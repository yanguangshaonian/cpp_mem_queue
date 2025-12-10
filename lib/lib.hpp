// //
// // Created by yanguangshaonian on 25-11-14.
// //
//
// #ifndef LIB_HPP
// #define LIB_HPP
//
// #include <string>
// #include <sys/mman.h>
// #include <unistd.h>
// #include <iostream>
// #include <atomic>
// #include <cstdlib>
// #include <cstring>
// #include <cerrno>
// #include <utility>
// #include <immintrin.h>
// #include <fcntl.h>
// #include <thread>
//
// using namespace std;
//
// constexpr size_t HUGE_PAGE_SIZE = 1024 * 1024 * 2;
// constexpr size_t CACHE_LINE_SIZE = 64;
//
// enum class ReadStatus {
//     SUCCESS,
//     NOT_READY,
//     OVERWRITTEN
// };
//
// template<class T>
// class alignas(CACHE_LINE_SIZE) PaddedValue {
//     public:
//         alignas(alignof(T)) std::byte data_bytes[sizeof(T)]{}s;
//
//         T& value() {
//             return *reinterpret_cast<T*>(&data_bytes);
//         }
//         const T& value() const {
//             return *reinterpret_cast<const T*>(&data_bytes);
//         }
//         PaddedValue() = default;
//         ~PaddedValue() = default;
// };
//
//
// template<class T, uint64_t CNT>
// class SharedDataStore {
//     static_assert(CNT && !(CNT & (CNT - 1)), "CNT 必须是 2 的幂次方");
//     alignas(CACHE_LINE_SIZE) atomic<uint64_t> size{0};
//     PaddedValue<T> data[CNT];
//
//
//     [[nodiscard]] uint64_t get_current_size() const {
//         return this->size.load(memory_order_acquire);
//     }
//
//     static uint64_t mask(const uint64_t val) {
//         return val & (CNT - 1);
//     }
//
//     public:
//         template<class Writer>
//         void write(Writer writer) {
//             const uint64_t current_idx = this->size.load(memory_order_relaxed);
//             auto& data_ref = this->data[mask(current_idx)].value();
//
//             if constexpr (!std::is_trivially_destructible_v<T>) {
//                 if (current_idx >= CNT) {
//                     std::destroy_at(&data_ref);
//                 }
//             }
//
//             writer(data_ref);
//             this->size.store(current_idx + 1, memory_order_release);
//         }
//
//         template<class Reader>
//         ReadStatus read(const uint64_t local_read_idx, Reader reader) {
//             const auto current_size = this->get_current_size();
//
//             if (local_read_idx < current_size) {
//                 const auto oldest_available_idx = (current_size >= CNT) ? current_size - CNT : 0;
//                 if (local_read_idx < oldest_available_idx) {
//                     return ReadStatus::OVERWRITTEN;
//                 }
//                 const auto& data_ref = this->data[mask(local_read_idx)].value();
//                 reader(data_ref);
//                 return ReadStatus::SUCCESS;
//             }
//
//             _mm_pause();
//             return ReadStatus::NOT_READY;
//         }
// };
//
//
// template<class T, uint32_t CNT>
// class MemoryStorage {
//     using StoreType = SharedDataStore<T, CNT>;
//     int32_t shm_fd=-1;
//     const uint64_t SHM_READY_MAGIC = 0xDEADBEEFCAFEBABE;
//
//     struct ShmLayout {
//         alignas(CACHE_LINE_SIZE) atomic<uint64_t> ready_flag{0};
//         StoreType store;
//     };
//
//     public:
//         string storage_name;
//         const uint64_t shared_data_store_size = sizeof(ShmLayout);
//         explicit MemoryStorage(string storage_name): storage_name(move(storage_name)) {}
//
//         StoreType* create() {
//             if (shm_unlink(this->storage_name.c_str()) == -1) {
//                 cerr << "shm_unlink调用失败: " << strerror(errno) << endl;
//             }
//
//             this->shm_fd = shm_open(this->storage_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
//             if (shm_fd == -1) {
//                 cerr << "shm_open调用失败: " << strerror(errno) << endl;
//                 return nullptr;
//             }
//
//             if (ftruncate(this->shm_fd, shared_data_store_size) == -1) {
//                 cerr << "ftruncate调用失败: " << strerror(errno) << endl;
//                 close(this->shm_fd);
//                 return nullptr;
//             }
//
//             auto mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB, this->shm_fd, 0);
//             if (mapped_ptr == MAP_FAILED) {
//                 mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED, this->shm_fd, 0);
//             }
//             if (mapped_ptr == MAP_FAILED) {
//                 close(this->shm_fd);
//                 shm_unlink(this->storage_name.c_str());
//                 cerr << "mmap调用失败: " << strerror(errno) << endl;
//                 return nullptr;
//             }
//
//             auto layout = static_cast<ShmLayout*>(mapped_ptr);
//             new (layout) ShmLayout();
//             layout->ready_flag.store(SHM_READY_MAGIC, memory_order_release);
//
//             cout << "创建 " << this->storage_name << " " << shared_data_store_size << " bytes 共享内存"<< endl;
//             return &(layout->store);
//         }
//
//
//         StoreType* attach() {
//             this->shm_fd = shm_open(this->storage_name.c_str(), O_RDWR, 0660);
//             if (this->shm_fd == -1) {
//                 cerr << "shm_open (attach) 调用失败: " << strerror(errno) << endl;
//                 return nullptr;
//             }
//
//             auto mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB, this->shm_fd, 0);
//             if (mapped_ptr == MAP_FAILED) {
//                 mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED, this->shm_fd, 0);
//             }
//             if (mapped_ptr == MAP_FAILED) {
//                 close(this->shm_fd);
//                 cerr << "mmap (attach) 调用失败: " << strerror(errno) << endl;
//                 return nullptr;
//             }
//
//             auto layout = static_cast<ShmLayout*>(mapped_ptr);
//             auto delay_cnt = 0;
//             while (layout->ready_flag.load(memory_order_acquire) != SHM_READY_MAGIC) {
//                 _mm_pause();
//                 delay_cnt += 1;
//                 this_thread::yield();
//             }
//
//             cout << "附加次数统计: " << delay_cnt << endl;
//             cout << "附加 " << this->storage_name << " " << shared_data_store_size << " bytes 共享内存"<< endl;
//             return &(layout->store);
//         }
//
//
//         MemoryStorage(const MemoryStorage&) = delete;
//         MemoryStorage& operator=(const MemoryStorage&) = delete;
// };
//
//
//
// #endif //LIB_HPP
// // 我不在乎慢消费者, 我的内存足够大, 它会驻留在内存上很长的时间, 满消费者总是能追赶上生产者, 所以不会出现数据被覆盖, 我希望每个进程自己维护自己的读取下标, 每个进程都要读取到消息, 所以不用保存在 共享内存结构体中
// // 只有一个生产者, 可能会存在多个消费者, 但是消费者非任务窃取, 检查是否有性能问题


//
// //
// // Created by yanguangshaonian on 25-11-14.
// //
//
// #ifndef LIB_HPP
// #define LIB_HPP
//
// #include <string>
// #include <sys/mman.h>
// #include <unistd.h>
// #include <iostream>
// #include <atomic>
// #include <cstdlib>
// #include <cstring>
// #include <cerrno>
// #include <utility>
// #include <immintrin.h>
// #include <fcntl.h>
// #include <thread>
//
// using namespace std;
//
// constexpr size_t HUGE_PAGE_SIZE = 1024 * 1024 * 2;
// constexpr size_t CACHE_LINE_SIZE = 64;
//
// enum class ReadStatus {
//     SUCCESS,
//     NOT_READY,
//     OVERWRITTEN
// };
//
// enum class Role {
//     CONSUMER,
//     PUBLISHER
// };
//
// template<class T>
// class alignas(CACHE_LINE_SIZE) PaddedValue {
//     public:
//         alignas(alignof(T)) std::byte data_bytes[sizeof(T)]{};
//
//         T& value() {
//             return *reinterpret_cast<T*>(&data_bytes);
//         }
//         const T& value() const {
//             return *reinterpret_cast<const T*>(&data_bytes);
//         }
//         PaddedValue() = default;
//         ~PaddedValue() = default;
// };
//
//
// template<class T, uint64_t CNT>
// class SharedDataStore {
//     static_assert(CNT && !(CNT & (CNT - 1)), "CNT 必须是 2 的幂次方");
//     alignas(CACHE_LINE_SIZE) atomic<uint64_t> producer_idx{0};
//     alignas(CACHE_LINE_SIZE) atomic<uint64_t> consumer_visible_idx{0};
//
//     PaddedValue<T> data[CNT];
//
//     // 这个只能有消费者调用
//     [[nodiscard]] uint64_t get_consumer_visible_idx() const {
//         return this->consumer_visible_idx.load(memory_order_acquire);
//     }
//
//     static uint64_t mask(const uint64_t val) {
//         return val & (CNT - 1);
//     }
//
//     public:
//         template<class Writer>
//         void write(Writer writer) {
//             const uint64_t current_idx = this->producer_idx.load(memory_order_relaxed);
//             auto& data_ref = this->data[mask(current_idx)].value();
//
//             if constexpr (!std::is_trivially_destructible_v<T>) {
//                 if (current_idx >= CNT) {
//                     std::destroy_at(&data_ref);
//                 }
//             }
//             writer(data_ref);
//             this->producer_idx.store(current_idx + 1, memory_order_relaxed);
//         }
//
//         bool flush() {
//             const uint64_t current_producer_idx = this->producer_idx.load(memory_order_relaxed);
//             if (this->consumer_visible_idx.load(memory_order_relaxed) < current_producer_idx) {
//                 this->consumer_visible_idx.store(current_producer_idx, memory_order_release);
//                 return true;
//             }
//             return false;
//         }
//
//         template<class Reader>
//         ReadStatus read(const uint64_t local_read_idx, Reader reader) {
//             const auto consumer_visible_idx = this->get_consumer_visible_idx();
//
//             if (local_read_idx < consumer_visible_idx) {
//                 const auto oldest_available_idx = (consumer_visible_idx >= CNT) ? consumer_visible_idx - CNT : 0;
//                 if (local_read_idx < oldest_available_idx) {
//                     return ReadStatus::OVERWRITTEN;
//                 }
//                 const auto& data_ref = this->data[mask(local_read_idx)].value();
//                 reader(data_ref);
//                 return ReadStatus::SUCCESS;
//             }
//
//             _mm_pause();
//             return ReadStatus::NOT_READY;
//         }
// };
//
//
// template<class T, uint64_t CNT>
// class MemoryStorage {
//     using StoreType = SharedDataStore<T, CNT>;
//     int32_t shm_fd=-1;
//     const uint64_t SHM_READY_MAGIC = 0xDEADBEEFCAFEBABE;
//     std::atomic<bool> running{false};  // 线程停止标志
//     thread flush_thread{};               // 刷新线程
//
//     struct ShmLayout {
//         alignas(CACHE_LINE_SIZE) atomic<uint64_t> ready_flag{0};
//         StoreType store;
//     };
//     ShmLayout* layout_ptr{nullptr};
//     Role role{};
//
//     void publisher_loop() {
//         auto cnt = 0;
//         while (this->running.load(memory_order_relaxed)) {
//             // 如果有数据
//             if (this->layout_ptr->store.flush()) {
//                 cnt = 0;
//             } else {
//                 // 指数退避
//                 cnt = (cnt << 1) & 4095;
//                 for (int i = 0; i < cnt; ++i) {
//                     if (this->running.load(memory_order_relaxed)) {
//                         _mm_pause();
//                     } else {
//                         break;
//                     }
//                 }
//             }
//         }
//     }
//
//     public:
//         string storage_name;
//         const uint64_t shared_data_store_size = sizeof(ShmLayout);
//         explicit MemoryStorage(string storage_name): storage_name(move(storage_name)) {}
//
//         StoreType* create() {
//             this->role = Role::PUBLISHER;
//             if (shm_unlink(this->storage_name.c_str()) == -1) {
//                 cerr << "shm_unlink调用失败: " << strerror(errno) << endl;
//             }
//
//             this->shm_fd = shm_open(this->storage_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
//             if (shm_fd == -1) {
//                 cerr << "shm_open调用失败: " << strerror(errno) << endl;
//                 return nullptr;
//             }
//
//             if (ftruncate(this->shm_fd, shared_data_store_size) == -1) {
//                 cerr << "ftruncate调用失败: " << strerror(errno) << endl;
//                 close(this->shm_fd);
//                 return nullptr;
//             }
//
//             auto mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB, this->shm_fd, 0);
//             if (mapped_ptr == MAP_FAILED) {
//                 mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED, this->shm_fd, 0);
//             }
//             if (mapped_ptr == MAP_FAILED) {
//                 close(this->shm_fd);
//                 shm_unlink(this->storage_name.c_str());
//                 cerr << "mmap调用失败: " << strerror(errno) << endl;
//                 return nullptr;
//             }
//
//             this->layout_ptr = static_cast<ShmLayout*>(mapped_ptr);
//             new (this->layout_ptr) ShmLayout();
//             this->layout_ptr->ready_flag.store(SHM_READY_MAGIC, memory_order_release);
//             this->running.store(true, memory_order_relaxed);
//             this->flush_thread = thread([this]() {
//                 this->publisher_loop();
//             });
//
//             cout << "创建 " << this->storage_name << " " << shared_data_store_size << " bytes 共享内存"<< endl;
//
//             return &(this->layout_ptr->store);
//         }
//
//         StoreType* attach() {
//             this->role = Role::CONSUMER;
//             this->shm_fd = shm_open(this->storage_name.c_str(), O_RDWR, 0660);
//             if (this->shm_fd == -1) {
//                 cerr << "shm_open (attach) 调用失败: " << strerror(errno) << endl;
//                 return nullptr;
//             }
//
//             auto mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB, this->shm_fd, 0);
//             if (mapped_ptr == MAP_FAILED) {
//                 mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED, this->shm_fd, 0);
//             }
//             if (mapped_ptr == MAP_FAILED) {
//                 close(this->shm_fd);
//                 cerr << "mmap (attach) 调用失败: " << strerror(errno) << endl;
//                 return nullptr;
//             }
//
//             this->layout_ptr = static_cast<ShmLayout*>(mapped_ptr);
//             auto delay_cnt = 0;
//             while (this->layout_ptr->ready_flag.load(memory_order_acquire) != SHM_READY_MAGIC) {
//                 _mm_pause();
//                 delay_cnt += 1;
//                 this_thread::yield();
//                 // todo 调用超时增加
//             }
//
//             cout << "附加次数统计: " << delay_cnt << endl;
//             cout << "附加 " << this->storage_name << " " << shared_data_store_size << " bytes 共享内存"<< endl;
//             return &(this->layout_ptr->store);
//         }
//
//
//         MemoryStorage(const MemoryStorage&) = delete;
//         MemoryStorage& operator=(const MemoryStorage&) = delete;
//
//         ~MemoryStorage() {
//             if (this->role == Role::PUBLISHER) {
//                 this->running.store(false, memory_order_relaxed);
//                 if (this->flush_thread.joinable()) {
//                     this->flush_thread.join();
//                     cout << "flush_thread 线程停止" << endl;
//                 }
//
//                 if (this->layout_ptr) {
//                     munmap(this->layout_ptr, shared_data_store_size);
//                     cout << "生产者 munmap 已完成。" << endl;
//                 }
//
//                 if (shm_fd != -1) {
//                     close(shm_fd);
//                 }
//             }
//         }
// };
//
//
//
// #endif //LIB_HPP


//
// Created by yanguangshaonian on 25-11-14.
//

#ifndef LIB_HPP
#define LIB_HPP
#include <chrono>
#pragma pack(push)
#pragma pack()

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

enum class ReadStatus {
    SUCCESS,
    NOT_READY,
    OVERWRITTEN,
};

enum class Role {
    CONSUMER,
    PUBLISHER
};

inline int futex_wake(std::atomic<int>* addr, int count) {
    return syscall(SYS_futex, reinterpret_cast<int*>(addr), FUTEX_WAKE, count, nullptr, nullptr, 0);
}

template<class T>
class alignas(CACHE_LINE_SIZE) PaddedValue {
    public:
        alignas(alignof(T)) std::byte data_bytes[sizeof(T)]{};

        T& value() {
            return *reinterpret_cast<T*>(&data_bytes);
        }

        const T& value() const {
            return *reinterpret_cast<const T*>(&data_bytes);
        }

        PaddedValue() = default;
        ~PaddedValue() = default;
};

template<class T, uint64_t CNT>
class SharedDataStore {
        static_assert(CNT && !(CNT & (CNT - 1)), "CNT 必须是 2 的幂次方");
        static_assert(std::is_trivially_copyable_v<T>, "T 必须可以安全地通过内存复制");
        static_assert(std::is_trivially_destructible_v<T>, "T 不允许为指针");

        alignas(CACHE_LINE_SIZE) atomic<uint64_t> producer_idx{0};
        alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> futex_flag{0};
        PaddedValue<T> data[CNT];

        static uint64_t mask(const uint64_t val) {
            return val & (CNT - 1);
        }

    public:
        [[nodiscard]] uint64_t get_current_idx() const {
            return this->producer_idx.load(memory_order_acquire);
        }

        template<class Writer>
        void write(Writer writer) {
            const uint64_t current_idx = this->producer_idx.load(memory_order_relaxed);
            auto& data_ref = this->data[mask(current_idx)].value();

            if constexpr (!std::is_trivially_destructible_v<T>) {
                if (current_idx >= CNT) {
                    std::destroy_at(&data_ref);
                }
            }
            writer(data_ref);
            this->producer_idx.store(current_idx + 1, memory_order_release);
        }

        template<class Writer>
        void write_wake(Writer writer) {
            this->write(writer);
            // 唤醒
            this->futex_flag.fetch_add(1, std::memory_order_release);
            syscall(SYS_futex, &this->futex_flag, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
        }

        template<class Reader>
        ReadStatus read(const uint64_t local_read_idx, Reader reader) {
            uint32_t spin = 2;
            constexpr auto MAX_SPIN = 128;
            while (true) {
                const auto current_idx = this->get_current_idx();
                if (local_read_idx < current_idx) {
                    const auto oldest_available_idx = (current_idx >= CNT) ? current_idx - CNT : 0;
                    if (local_read_idx < oldest_available_idx) {
                        return ReadStatus::OVERWRITTEN;
                    }
                    const auto& data_ref = this->data[mask(local_read_idx)].value();
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
        ReadStatus read_wait(uint64_t local_read_idx, Reader reader, int delay_time_us = -1) {
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
                if (this->get_current_idx() > local_read_idx) {
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

        template<class ReaderFunc>
        ReadStatus read_umwait(uint64_t local_read_idx, ReaderFunc reader, int timeout_us = -1, uint32_t state = 1) {
            constexpr uint64_t CYCLES_PER_US = 2500;
            bool infinite_wait = (timeout_us < 0);
            uint64_t tsc_deadline = 0;

            if (!infinite_wait) {
                tsc_deadline = __rdtsc() + (timeout_us * CYCLES_PER_US);
            } else {
                // 设置为最大值，但注意 umwait 有 OS 级的最大休眠时间限制
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
                if (this->get_current_idx() > local_read_idx) {
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

        ShmLayout* layout_ptr{nullptr};
        Role role{};

    public:
        string storage_name;
        const uint64_t shared_data_store_size = sizeof(ShmLayout);
        MemoryStorage() = default;

        StoreType* create(string& storage_name) {
            this->storage_name = move(storage_name);

            if (geteuid() != 0) {
                cerr << "create 需要 root 权限，否则无法进行 shm_open / mmap / MAP_HUGETLB 等操作" << endl;
                return nullptr;
            }

            this->role = Role::PUBLISHER;
            if (shm_unlink(this->storage_name.c_str()) == -1) {
                cerr << "shm_unlink调用失败: " << strerror(errno) << endl;
            }

            this->shm_fd = shm_open(this->storage_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
            if (shm_fd == -1) {
                cerr << "shm_open调用失败: " << strerror(errno) << endl;
                return nullptr;
            }

            if (ftruncate(this->shm_fd, shared_data_store_size) == -1) {
                cerr << "ftruncate调用失败: " << strerror(errno) << endl;
                close(this->shm_fd);
                return nullptr;
            }

            auto mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB,
                                   this->shm_fd, 0);
            if (mapped_ptr == MAP_FAILED) {
                mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED, this->shm_fd, 0);
            }
            if (mapped_ptr == MAP_FAILED) {
                close(this->shm_fd);
                shm_unlink(this->storage_name.c_str());
                cerr << "mmap调用失败: " << strerror(errno) << endl;
                return nullptr;
            }

            this->layout_ptr = static_cast<ShmLayout*>(mapped_ptr);
            new (this->layout_ptr) ShmLayout();
            this->layout_ptr->ready_flag.store(SHM_READY_MAGIC, memory_order_release);

            cout << "创建 " << this->storage_name << " " << shared_data_store_size << " bytes 共享内存" << endl;

            return &(this->layout_ptr->store);
        }

        StoreType* attach(string& storage_name, const uint32_t delay_time_ms) {
            this->storage_name = move(storage_name);
            if (geteuid() != 0) {
                cerr << "attach 需要 root 权限，否则无法进行 shm_open / mmap / MAP_HUGETLB 等操作" << endl;
                return nullptr;
            }

            this->role = Role::CONSUMER;
            this->shm_fd = shm_open(this->storage_name.c_str(), O_RDWR, 0660);
            if (this->shm_fd == -1) {
                cerr << "shm_open (attach) 调用失败: " << strerror(errno) << endl;
                return nullptr;
            }

            auto mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB,
                                   this->shm_fd, 0);
            if (mapped_ptr == MAP_FAILED) {
                mapped_ptr = mmap(nullptr, shared_data_store_size, PROT_READ | PROT_WRITE, MAP_SHARED, this->shm_fd, 0);
            }
            if (mapped_ptr == MAP_FAILED) {
                close(this->shm_fd);
                cerr << "mmap (attach) 调用失败: " << strerror(errno) << endl;
                return nullptr;
            }

            this->layout_ptr = static_cast<ShmLayout*>(mapped_ptr);

            auto start_time = steady_clock::now();
            auto end_time = start_time + milliseconds(delay_time_ms);
            while (this->layout_ptr->ready_flag.load(memory_order_acquire) != SHM_READY_MAGIC) {
                _mm_pause();
                this_thread::yield();
                if (std::chrono::steady_clock::now() > end_time) {
                    cerr << "attach() 等待 ready_flag 超过 " << delay_time_ms << "ms" << endl;
                    return nullptr;
                }
                this_thread::sleep_for(milliseconds(1));
            }

            cout << "附加耗时: "
                 << duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count()
                 << "ms" << endl;
            cout << "附加 " << this->storage_name << " " << shared_data_store_size << " bytes 共享内存" << endl;
            return &(this->layout_ptr->store);
        }

        MemoryStorage(const MemoryStorage&) = delete;
        MemoryStorage& operator=(const MemoryStorage&) = delete;

        ~MemoryStorage() {
            if (this->layout_ptr) {
                munmap(this->layout_ptr, shared_data_store_size);
                cout << (this->role == Role::PUBLISHER ? "生产者 " : "消费者 ") << this->storage_name
                     << " munmap 已完成。" << endl;
            }

            if (shm_fd != -1) {
                close(shm_fd);
            }

            if (this->role == Role::PUBLISHER) {
                shm_unlink(this->storage_name.c_str());
                cout << "生产者 " << this->storage_name << " shm_unlink 已完成。" << endl;
            }
        }
};

#pragma pack(pop)

#endif //LIB_HPP