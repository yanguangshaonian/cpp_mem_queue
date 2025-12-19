//
// Created by yanguangshaonian on 25-11-14.
//

#ifndef LIB_HPP
#define LIB_HPP
#pragma pack(push)
#pragma pack()

#include <chrono>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <iostream>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <utility>
#include <immintrin.h>
#include <fcntl.h>
#include <thread>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <climits>
#include <cstdint>

using namespace std;
using namespace std::chrono;

constexpr size_t HUGE_PAGE_SIZE = 1024 * 1024 * 2;
constexpr size_t CACHE_LINE_SIZE = 64;
constexpr u_int16_t MAX_SPIN = 128;

enum class ReadStatus {
    SUCCESS,
    NOT_READY,
    OVERWRITTEN,
};

enum class Role {
    WRITER,
    READER
};

template<class T>
class alignas(CACHE_LINE_SIZE) PaddedValue {
    public:
        T value;
};

template<class T, uint64_t CNT>
class SharedDataStore {
        static_assert(CNT && !(CNT & (CNT - 1)), "CNT 必须是 2 的幂次方");
        static_assert(std::is_trivially_copyable_v<T>, "T 必须可以安全地通过内存复制");
        static_assert(std::is_trivially_destructible_v<T>, "T 不允许为指针");

        alignas(CACHE_LINE_SIZE) atomic<uint64_t> producer_idx{0};
        alignas(CACHE_LINE_SIZE) atomic<uint64_t> consumed_idx{0};
        alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> futex_flag{0};
        alignas(CACHE_LINE_SIZE) PaddedValue<T> data[CNT];

        static uint64_t mask(const uint64_t val) {
            return val & (CNT - 1);
        }

    public:
        [[nodiscard]] uint64_t get_producer_idx() const {
            return this->producer_idx.load(memory_order_acquire);
        }

        [[nodiscard]] uint64_t get_consumed_idx() const {
            return this->consumed_idx.load(memory_order_acquire);
        }

        template<class Writer>
        inline __attribute__((always_inline)) void write(Writer&& writer) {
            const uint64_t producer_idx = this->producer_idx.load(memory_order_relaxed);
            auto& data_ref = this->data[mask(producer_idx)].value;

            if constexpr (!std::is_trivially_destructible_v<T>) {
                if (producer_idx >= CNT) {
                    std::destroy_at(&data_ref);
                }
            }
            writer(data_ref);
            this->producer_idx.store(producer_idx + 1, memory_order_release);
        }

        template<class Writer>
        inline __attribute__((always_inline)) void write_wake(Writer&& writer) {
            this->write(writer);
            // 唤醒
            this->futex_flag.fetch_add(1, std::memory_order_release);
            syscall(SYS_futex, &this->futex_flag, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
        }

        template<class Reader>
        inline __attribute__((always_inline)) ReadStatus read(const uint64_t local_read_idx, Reader&& reader) noexcept {
            u_int16_t spin = 2;
            while (true) {
                const auto producer_idx = this->get_producer_idx();
                if (local_read_idx < producer_idx) {
                    const auto oldest_available_idx = (producer_idx >= CNT) ? producer_idx - CNT : 0;
                    if (local_read_idx < oldest_available_idx) {
                        return ReadStatus::OVERWRITTEN;
                    }
                    const auto& data_ref = this->data[mask(local_read_idx)].value;
                    reader(data_ref);
                    return ReadStatus::SUCCESS;
                }

                if (spin > MAX_SPIN) {
                    return ReadStatus::NOT_READY;
                }

                for (auto i = 0; i < spin; ++i) {
                    _mm_pause();
                }
                spin <<= 1;
            }
        }

        template<class Reader>
        inline __attribute__((always_inline)) ReadStatus read_wait(uint64_t local_read_idx, Reader&& reader,
                                                                   int32_t delay_time_us = -1) noexcept {
            bool infinite_wait = (delay_time_us < 0);
            auto start_time = steady_clock::now();
            auto end_time = start_time + microseconds(delay_time_us);

            while (true) {
                auto read_status = this->read(local_read_idx, reader);

                // 初次读取后的校验
                if (read_status != ReadStatus::NOT_READY) {
                    return read_status;
                }

                auto expected = futex_flag.load(std::memory_order_acquire);

                // 二次校验
                if (this->get_producer_idx() > local_read_idx) {
                    continue;
                }

                struct timespec ts;
                struct timespec* ts_ptr = nullptr; // 默认为 nullptr (无限等待)
                if (!infinite_wait) {
                    auto now = steady_clock::now();
                    if (now >= end_time) {
                        return ReadStatus::NOT_READY; // 确实超时了
                    }

                    // 计算剩余的微秒数
                    auto remaining_ns = duration_cast<nanoseconds>(end_time - now).count();
                    ts.tv_sec = remaining_ns / 1000000000LL;
                    ts.tv_nsec = remaining_ns % 1000000000LL;
                    ts_ptr = &ts;
                }

                syscall(SYS_futex, &futex_flag, FUTEX_WAIT, expected, ts_ptr, nullptr, 0);
            }
        }

        template<class Reader>
        inline __attribute__((always_inline)) ReadStatus read_umwait(uint64_t local_read_idx, Reader&& reader,
                                                                     int timeout_us = -1, uint32_t state = 1) noexcept {
            constexpr uint64_t CYCLES_PER_US = 2500;
            bool infinite_wait = (timeout_us < 0);
            uint64_t tsc_deadline = 0;

            if (!infinite_wait) {
                tsc_deadline = __rdtsc() + (timeout_us * CYCLES_PER_US);
            } else {
                tsc_deadline = ~0ULL;
            }

            while (true) {
                auto read_status = this->read(local_read_idx, reader);

                // 初次读取后的校验
                if (read_status != ReadStatus::NOT_READY) {
                    return read_status;
                }

                _umonitor(&this->producer_idx);

                // 二次校验
                if (this->get_producer_idx() > local_read_idx) {
                    continue;
                }

                if (!infinite_wait) {
                    if (__rdtsc() >= tsc_deadline) {
                        return ReadStatus::NOT_READY;
                    }
                }
                _umwait(state, tsc_deadline);
            };
        };

        template<class Reader>
        inline __attribute__((always_inline)) ReadStatus read_competing(const uint64_t local_read_idx,
                                                                        Reader&& reader) noexcept {
            u_int16_t spin = 2;
            while (true) {
                const auto producer_idx = this->get_producer_idx();
                if (local_read_idx < producer_idx) {
                    const auto oldest_available_idx = (producer_idx >= CNT) ? producer_idx - CNT : 0;
                    if (local_read_idx < oldest_available_idx) {
                        return ReadStatus::OVERWRITTEN;
                    }

                    // if (this->consumed_idx.load(std::memory_order_relaxed) > local_read_idx) {
                    //     return ReadStatus::SUCCESS;
                    // }
                    uint64_t expected = local_read_idx;
                    auto is_owner = this->consumed_idx.compare_exchange_strong(
                        expected, local_read_idx + 1, std::memory_order_acq_rel, std::memory_order_relaxed);
                    const auto& data_ref = this->data[mask(local_read_idx)].value;
                    reader(data_ref, is_owner);
                    return ReadStatus::SUCCESS;
                }

                if (spin > MAX_SPIN) {
                    return ReadStatus::NOT_READY;
                }

                for (auto i = 0; i < spin; ++i) {
                    _mm_pause();
                }
                spin <<= 1;
            }
        }

        template<class Reader>
        inline __attribute__((always_inline)) ReadStatus read_wait_competing(uint64_t local_read_idx, Reader&& reader,
                                                                             int32_t delay_time_us = -1) noexcept {
            bool infinite_wait = (delay_time_us < 0);
            auto start_time = steady_clock::now();
            auto end_time = start_time + microseconds(delay_time_us);

            while (true) {
                auto read_status = this->read_competing(local_read_idx, reader);

                if (read_status != ReadStatus::NOT_READY) {
                    return read_status;
                }

                auto expected = futex_flag.load(std::memory_order_acquire);

                if (this->get_producer_idx() > local_read_idx) {
                    continue;
                }

                struct timespec ts;
                struct timespec* ts_ptr = nullptr;
                if (!infinite_wait) {
                    auto now = steady_clock::now();
                    if (now >= end_time) {
                        return ReadStatus::NOT_READY; // 确实超时了
                    }
                    auto remaining_ns = duration_cast<nanoseconds>(end_time - now).count();
                    ts.tv_sec = remaining_ns / 1000000000LL;
                    ts.tv_nsec = remaining_ns % 1000000000LL;
                    ts_ptr = &ts;
                }
                syscall(SYS_futex, &futex_flag, FUTEX_WAIT, expected, ts_ptr, nullptr, 0);
            }
        }
};

template<class T, uint64_t CNT>
class MemoryStorage {
    protected:
        using StoreType = SharedDataStore<T, CNT>;
        int32_t shm_fd = -1;
        const uint64_t SHM_READY_MAGIC = 0xDEADBEEFCAFEBABE;

        class ShmLayout {
            public:
                alignas(CACHE_LINE_SIZE) atomic<uint64_t> ready_flag{0};
                StoreType store;
        };

        ShmLayout* __restrict__ layout_ptr{nullptr};
        Role role{};

        // 优先尝试大页, 失败降级为普通页
        void* map_memory_segment() {
            auto ptr = mmap(nullptr, layout_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB, this->shm_fd, 0);
            if (ptr == MAP_FAILED) {
                cout << "mmap 大页分配错误: " << strerror(errno) << ", 尝试回退" << endl;
                ptr = mmap(nullptr, layout_size, PROT_READ | PROT_WRITE, MAP_SHARED, this->shm_fd, 0);
            }
            return ptr;
        }

        int try_join_existing() {
            this->shm_fd = shm_open(this->storage_name.c_str(), O_RDWR, 0660);
            if (this->shm_fd == -1) {
                return -1; // 文件不存在，去创建
            }

            void* ptr = map_memory_segment();
            if (ptr == MAP_FAILED) {
                cerr << "mmap 失败: " << strerror(errno) << endl;
                close(this->shm_fd);
                return -1;
            }

            auto* temp_layout = static_cast<ShmLayout*>(ptr);

            // 等待初始化完成(极短窗口)
            auto start = steady_clock::now();
            while (temp_layout->ready_flag.load(memory_order_acquire) != SHM_READY_MAGIC) {
                if (steady_clock::now() - start > milliseconds(100)) {
                    // 超时仍未就绪，说明是残留的坏文件，执行清理
                    cerr << ">> [警告] 检测到残留的损坏文件 (Magic无效), 正在清理..." << endl;
                    munmap(ptr, layout_size);
                    close(this->shm_fd);
                    shm_unlink(this->storage_name.c_str());
                    return -2; // 文件存在但无效(已执行unlink)
                }
                this_thread::sleep_for(milliseconds(1));
            }

            cout << ">> [复用成功] " << this->storage_name
                 << " attached. Index: " << temp_layout->store.get_producer_idx() << endl;
            this->layout_ptr = temp_layout;
            return 0;
        }

        bool try_create_new() {
            // O_EXCL 保证原子性：如果文件已存在则报错 EEXIST
            this->shm_fd = shm_open(this->storage_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
            if (this->shm_fd == -1) {
                return false;
            }

            if (ftruncate(this->shm_fd, layout_size) == -1) {
                cerr << "ftruncate 失败: " << strerror(errno) << endl;
                close(this->shm_fd);
                shm_unlink(this->storage_name.c_str());
                return false;
            }

            void* ptr = map_memory_segment();
            if (ptr == MAP_FAILED) {
                cerr << "mmap 失败: " << strerror(errno) << endl;
                close(this->shm_fd);
                shm_unlink(this->storage_name.c_str());
                return false;
            }

            this->layout_ptr = static_cast<ShmLayout*>(ptr);
            // 初始化
            new (this->layout_ptr) ShmLayout();
            this->layout_ptr->ready_flag.store(SHM_READY_MAGIC, memory_order_release);

            cout << ">> [新建成功] " << this->storage_name << " created." << endl;
            return true;
        }

    public:
        string storage_name;
        const uint64_t layout_size = sizeof(ShmLayout);
        MemoryStorage() = default;
        MemoryStorage(const MemoryStorage&) = delete;
        MemoryStorage& operator=(const MemoryStorage&) = delete;

        void build(string storage_name, Role role) {
            this->storage_name = storage_name;
            this->role = role;

            if (geteuid() != 0) {
                throw std::runtime_error("Critical Error: 需要 root 权限以使用 HugePage/mmap");
            }

            // 重试循环：处理并发启动时的竞争
            int retries = 3;
            while (retries != 0) {
                retries -= 1;

                // 复用
                int join_ret = try_join_existing();
                if (join_ret == 0) {
                    return;
                }

                // 新建
                if (try_create_new()) {
                    return;
                }

                // 有些异常
                if (errno == EEXIST) {
                    cout << ">> [并发竞争] 检测到其他进程刚刚创建了文件，重试..." << endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }

                // 其他错误
                throw std::runtime_error("shm_open 致命错误 (" + to_string(errno) + "): " + strerror(errno));
            }
            throw std::runtime_error("初始化超时: " + storage_name + " 存在严重的并发启动竞争");
        }

        inline __attribute__((always_inline)) StoreType& get_store() const {
            return this->layout_ptr->store;
        }

        ~MemoryStorage() {
            auto role = this->role == Role::WRITER ? "WRITER " : "READER ";
            if (this->layout_ptr) {
                munmap(this->layout_ptr, layout_size);
                cout << role << this->storage_name << " munmap 已完成" << endl;
            }

            if (shm_fd != -1) {
                close(shm_fd);
                cout << role << this->storage_name << " close fd 已完成" << endl;
            }
        }
};

#pragma pack(pop)

#endif //LIB_HPP