//
// Created by yanguangshaonian on 25-11-14.
//

#ifndef MEM_QUEUE_HPP
#define MEM_QUEUE_HPP
#pragma pack(push)
#pragma pack()
#include <iomanip>
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

namespace mem_queue {
    using namespace std;
    using namespace std::chrono;

    constexpr size_t HUGE_PAGE_SIZE = 2097152; // 2MB
    constexpr size_t CACHE_LINE_SIZE = 64;
    constexpr size_t MIN_MAP_SIZE = 4096; // 4KB
    constexpr uint16_t MAX_SPIN = 128;
    constexpr uint64_t SHM_READY_MAGIC = 0xDEADBEEFCAFEBABE;

    enum class ReadStatus {
        SUCCESS,
        NOT_READY,
        OVERWRITTEN, // 读太慢，数据已被覆盖
    };

    enum class Role {
        WRITER,
        READER
    };

    // ----------------------------------------------------------------
    // 辅助工具: 向上对齐到 2MB
    // ----------------------------------------------------------------
    inline uint64_t align_to_huge_page(uint64_t size) {
        return (size + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
    }

    // ----------------------------------------------------------------
    // 计算最近的 2 的幂次方 (例如: 100 -> 128)
    // ----------------------------------------------------------------
    inline uint64_t next_pow2(uint64_t v) {
        if (v == 0)
            return 1;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
    }

    // ----------------------------------------------------------------
    // 数据载体
    // ----------------------------------------------------------------
    template<class T, size_t ALIGN = CACHE_LINE_SIZE>
    class alignas(ALIGN) PaddedValue {
        public:
            T value;
    };

    // ----------------------------------------------------------------
    // 元数据头 (Metadata)
    // 包含队列控制变量，放在共享内存起始处
    // ----------------------------------------------------------------
    class alignas(CACHE_LINE_SIZE) ShmHeader {
        public:
            // 核心控制变量 (需保持缓存行对齐以避免伪共享)
            alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> producer_idx{0}; // 生产者索引
            alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> consumed_idx{0}; // 仅用于 read_competing 任务窃取模式
            alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> futex_flag{0};   // 仅用于 futex 唤醒模式

            volatile uint64_t magic_num;

            // 静态配置
            uint64_t element_count;       // 逻辑元素数量(必须是2的幂)
            uint64_t element_size;        // 单个元素大小
            uint64_t padded_element_size; // 填充后的元素大小
            uint64_t aligned_file_size;   // 2MB 对齐后的文件总大小 (用于 mmap)
    };

    // ----------------------------------------------------------------
    // 队列视图 (View)
    // 负责所有的逻辑操作 (Lock-Free RingBuffer)
    // ----------------------------------------------------------------
    template<class T, size_t ALIGN = CACHE_LINE_SIZE>
    class SharedQueueView {
        private:
            static_assert(std::is_trivially_copyable_v<T>, "T 必须可以安全地通过内存复制");
            static_assert(std::is_trivially_destructible_v<T>, "T 不允许为指针或包含复杂析构逻辑");

            ShmHeader* header = nullptr;
            PaddedValue<T, ALIGN>* data_ptr = nullptr;
            uint64_t capacity_mask = 0;

            inline uint64_t mask(const uint64_t val) const {
                return val & capacity_mask;
            }

        public:
            void init(uint8_t* base_addr) {
                this->header = reinterpret_cast<ShmHeader*>(base_addr);
                // 数据区紧跟在 Header 之后
                this->data_ptr = reinterpret_cast<PaddedValue<T, ALIGN>*>(base_addr + sizeof(ShmHeader));
                // capacity 必须是 2 的幂，由 MemoryStorage 保证
                this->capacity_mask = this->header->element_count - 1;
            }

            [[nodiscard]] uint64_t get_producer_idx() const {
                return this->header->producer_idx.load(std::memory_order_acquire);
            }

            [[nodiscard]] uint64_t get_consumed_idx() const {
                return this->header->consumed_idx.load(std::memory_order_acquire);
            }

            // ----------------------------------------------------------------
            // 写入逻辑 (单生产者安全)
            // ----------------------------------------------------------------
            template<class Writer>
            inline __attribute__((always_inline)) void write(Writer&& writer) {
                const uint64_t producer_idx = this->header->producer_idx.load(memory_order_relaxed);
                auto& data_ref = this->data_ptr[mask(producer_idx)].value;

                if constexpr (!std::is_trivially_destructible_v<T>) {
                    if (producer_idx >= header->element_count) {
                        std::destroy_at(&data_ref);
                    }
                }
                writer(data_ref);
                this->header->producer_idx.store(producer_idx + 1, memory_order_release);
            }

            template<class Writer>
            inline __attribute__((always_inline)) void write_wake(Writer&& writer) {
                this->write(writer);
                // 唤醒
                this->header->futex_flag.fetch_add(1, std::memory_order_release);
                syscall(SYS_futex, &this->header->futex_flag, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
            }

            // ----------------------------------------------------------------
            // 读取逻辑 (自旋模式)
            // ----------------------------------------------------------------
            template<class Reader>
            inline __attribute__((always_inline)) ReadStatus read(const uint64_t local_read_idx,
                                                                  Reader&& reader) noexcept {
                uint8_t delay = 0b0000'0001;
                while (true) {
                    // todo 这里密集写入的时候, 这边读取使用 ttas 反而性能下降, 可能是破坏cpu 流水线了, 其实也不用, 因为下面没数据的时候已经 pause了
                    const auto producer_idx = this->get_producer_idx();

                    if (local_read_idx < producer_idx) {
                        const auto cap = this->header->element_count;
                        const auto oldest_available_idx = (producer_idx >= cap) ? producer_idx - cap : 0;
                        if (local_read_idx < oldest_available_idx) {
                            return ReadStatus::OVERWRITTEN;
                        }
                        const auto& data_ref = this->data_ptr[mask(local_read_idx)].value;
                        reader(data_ref);
                        return ReadStatus::SUCCESS;
                    }

                    for (auto i = 0; i < delay; i += 1) {
                        asm volatile("pause" ::: "memory");
                    }

                    if (delay == 0b1000'0000) {
                        return ReadStatus::NOT_READY;
                    }
                    delay <<= 1;
                }
            }

            // ----------------------------------------------------------------
            // 读取逻辑 (Futex 等待模式)
            // ----------------------------------------------------------------
            template<class Reader>
            inline __attribute__((always_inline)) ReadStatus read_wait(uint64_t local_read_idx, Reader&& reader,
                                                                       int32_t delay_time_us = -1) noexcept {
                bool infinite_wait = (delay_time_us < 0);
                auto start_time = steady_clock::now();
                auto end_time = start_time + microseconds(delay_time_us);

                while (true) {
                    // 尝试直接读取
                    auto read_status = this->read(local_read_idx, reader);
                    if (read_status != ReadStatus::NOT_READY) {
                        return read_status;
                    }

                    // 准备等待, 先获取当前 flag
                    auto expected = this->header->futex_flag.load(std::memory_order_acquire);

                    // 二次校验
                    if (this->get_producer_idx() > local_read_idx) {
                        continue;
                    }

                    // 计算超时
                    struct timespec ts;
                    struct timespec* ts_ptr = nullptr;
                    if (!infinite_wait) {
                        auto now = steady_clock::now();
                        if (now >= end_time) {
                            return ReadStatus::NOT_READY;
                        }
                        auto remaining_ns = duration_cast<nanoseconds>(end_time - now).count();
                        ts.tv_sec = remaining_ns / 1000000000LL;
                        ts.tv_nsec = remaining_ns % 1000000000LL;
                        ts_ptr = &ts;
                    }

                    // 等待
                    syscall(SYS_futex, &this->header->futex_flag, FUTEX_WAIT, expected, ts_ptr, nullptr, 0);
                }
            }

            // template<class Reader>
            // inline __attribute__((always_inline)) ReadStatus read_umwait(uint64_t local_read_idx, Reader&& reader,
            //                                                              int timeout_us = -1,
            //                                                              uint32_t state = 1) noexcept {
            //     constexpr uint64_t CYCLES_PER_US = 2500;
            //     bool infinite_wait = (timeout_us < 0);
            //     uint64_t tsc_deadline = 0;

            //     if (!infinite_wait) {
            //         tsc_deadline = __rdtsc() + (timeout_us * CYCLES_PER_US);
            //     } else {
            //         tsc_deadline = ~0ULL;
            //     }

            //     while (true) {
            //         auto read_status = this->read(local_read_idx, reader);

            //         // 初次读取后的校验
            //         if (read_status != ReadStatus::NOT_READY) {
            //             return read_status;
            //         }

            //         _umonitor(&this->producer_idx);

            //         // 二次校验
            //         if (this->get_producer_idx() > local_read_idx) {
            //             continue;
            //         }

            //         if (!infinite_wait) {
            //             if (__rdtsc() >= tsc_deadline) {
            //                 return ReadStatus::NOT_READY;
            //             }
            //         }
            //         _umwait(state, tsc_deadline);
            //     };
            // };


            // ----------------------------------------------------------------
            // 竞争读取逻辑 (多消费者抢占模式)
            // ----------------------------------------------------------------
            template<class Reader>
            inline __attribute__((always_inline)) ReadStatus read_competing(const uint64_t local_read_idx,
                                                                            Reader&& reader) noexcept {
                // 该函数需要从0开始消费
                uint8_t delay = 0b0000'0001;
                while (true) {
                    const auto producer_idx = this->get_producer_idx();
                    if (local_read_idx < producer_idx) {
                        const auto cap = this->header->element_count;
                        const auto oldest_available_idx = (producer_idx >= cap) ? producer_idx - cap : 0;
                        if (local_read_idx < oldest_available_idx) {
                            const auto padding = cap >> 3; // cap / 8
                            auto safety_target = oldest_available_idx + padding;

                            if (safety_target > producer_idx) {
                                safety_target = producer_idx;
                            }
                            auto curr_consumed = this->header->consumed_idx.load(std::memory_order_relaxed);
                            while (curr_consumed < safety_target) {
                                if (this->header->consumed_idx.compare_exchange_weak(curr_consumed, safety_target,
                                                                                     std::memory_order_release,
                                                                                     std::memory_order_relaxed)) {
                                    break;
                                }
                            }
                            return ReadStatus::OVERWRITTEN;
                        }

                        uint64_t expected = local_read_idx;
                        auto is_owner = this->header->consumed_idx.compare_exchange_strong(
                            expected, local_read_idx + 1, std::memory_order_acq_rel, std::memory_order_relaxed);
                        const auto& data_ref = this->data_ptr[mask(local_read_idx)].value;
                        reader(data_ref, is_owner);
                        return ReadStatus::SUCCESS;
                    }

                    for (auto i = 0; i < delay; i += 1) {
                        asm volatile("pause" ::: "memory");
                    }

                    if (delay == 0b1000'0000) {
                        return ReadStatus::NOT_READY;
                    }
                    delay <<= 1;
                }
            }

            // ----------------------------------------------------------------
            // 竞争读取逻辑 (Futex 等待 + 多消费者抢占)
            // ----------------------------------------------------------------
            template<class Reader>
            inline __attribute__((always_inline)) ReadStatus read_wait_competing(uint64_t local_read_idx,
                                                                                 Reader&& reader,
                                                                                 int32_t delay_time_us = -1) noexcept {
                bool infinite_wait = (delay_time_us < 0);
                auto start_time = steady_clock::now();
                auto end_time = start_time + microseconds(delay_time_us);

                while (true) {
                    auto read_status = this->read_competing(local_read_idx, reader);

                    if (read_status != ReadStatus::NOT_READY) {
                        return read_status;
                    }

                    auto expected = this->header->futex_flag.load(std::memory_order_acquire);

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
                    syscall(SYS_futex, &this->header->futex_flag, FUTEX_WAIT, expected, ts_ptr, nullptr, 0);
                }
            }
    };

    // ----------------------------------------------------------------
    // 存储管理器
    // ----------------------------------------------------------------
    template<class T, size_t ALIGN = CACHE_LINE_SIZE>
    class MemoryStorage {
        private:
            enum class JoinResult {
                SUCCESS,
                FILE_NOT_FOUND,
                DATA_CORRUPT,
                TYPE_MISMATCH,
                SYSTEM_ERROR
            };

            string storage_name;
            SharedQueueView<T, ALIGN> view;

            // 资源的信息, 析构时候用
            int32_t shm_fd = -1;
            uint8_t* mapped_ptr = nullptr;
            uint64_t mapped_size = 0;

            void log_msg(const string& level, const string& msg) {
                // 获取简短的时间戳
                auto now = system_clock::now();
                auto in_time_t = system_clock::to_time_t(now);
                std::tm bt{};
                localtime_r(&in_time_t, &bt);

                cout << "[" << std::put_time(&bt, "%T") << "] "
                     << "[" << level << "] "
                     << "[" << storage_name << "] " << msg << endl;
            }

            uint8_t* map_memory_segment(size_t size, bool use_hugepage) {
                int flags = MAP_SHARED;
                if (use_hugepage) {
                    flags |= MAP_HUGETLB;
                }
                auto ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, this->shm_fd, 0);
                if (ptr == MAP_FAILED) {
                    return nullptr;
                }
                return reinterpret_cast<uint8_t*>(ptr);
            }

            JoinResult try_join_existing() {
                this->shm_fd = shm_open(this->storage_name.c_str(), O_RDWR, 0660);
                if (this->shm_fd == -1) {
                    if (errno == ENOENT)
                        return JoinResult::FILE_NOT_FOUND;
                    log_msg("ERROR", "shm_open 失败: " + string(strerror(errno)));
                    return JoinResult::SYSTEM_ERROR;
                }

                auto temp_ptr = map_memory_segment(MIN_MAP_SIZE, false);
                if (!temp_ptr) {
                    log_msg("ERROR", "Header mmap 失败: " + string(strerror(errno)));
                    close(this->shm_fd);
                    return JoinResult::SYSTEM_ERROR;
                }

                auto header = reinterpret_cast<ShmHeader*>(temp_ptr);


                // 等待初始化 2s
                int wait_count = 0;
                while (header->magic_num != SHM_READY_MAGIC) {
                    wait_count += 1;
                    if (wait_count > 2000) {
                        log_msg("ERROR", "Magic 校验超时 (文件损坏或初始化挂起)");
                        munmap(temp_ptr, MIN_MAP_SIZE);
                        close(this->shm_fd);

                        // 尝试清理无效文件, 以便后续可以重新创建
                        shm_unlink(this->storage_name.c_str());
                        return JoinResult::DATA_CORRUPT;
                    }
                    std::this_thread::sleep_for(milliseconds(1));
                    std::atomic_thread_fence(std::memory_order_acquire);
                }

                // 读取信息
                auto file_aligned_size = header->aligned_file_size;
                auto elem_cnt = header->element_count;
                auto elem_sz = header->element_size;

                if (elem_sz != sizeof(T)) {
                    log_msg("FATAL", "类型大小不匹配! 文件: " + to_string(elem_sz) + ", 本地: " + to_string(sizeof(T)));
                    munmap(temp_ptr, MIN_MAP_SIZE);
                    close(this->shm_fd);
                    return JoinResult::TYPE_MISMATCH;
                }
                if (header->padded_element_size != sizeof(PaddedValue<T, ALIGN>)) {
                    log_msg("FATAL", "填充大小不匹配! 文件: " + to_string(header->padded_element_size) +
                                         ", 本地: " + to_string(sizeof(PaddedValue<T, ALIGN>)));
                    munmap(temp_ptr, MIN_MAP_SIZE);
                    close(this->shm_fd);
                    return JoinResult::TYPE_MISMATCH;
                }

                // 解除临时映射, 然后完整映射
                munmap(temp_ptr, MIN_MAP_SIZE);
                this->mapped_ptr = map_memory_segment(file_aligned_size, true);

                if (this->mapped_ptr == nullptr) {
                    log_msg("WARN", "HugePage mmap 失败 (" + string(strerror(errno)) + "), 降级使用 4KB 页");
                    this->mapped_ptr = map_memory_segment(file_aligned_size, false);
                    if (this->mapped_ptr == nullptr) {
                        log_msg("ERROR", "降级 mmap 也失败: " + string(strerror(errno)));
                        close(this->shm_fd);
                        return JoinResult::SYSTEM_ERROR;
                    }
                }

                this->mapped_size = file_aligned_size;
                this->view.init(this->mapped_ptr);

                stringstream ss;
                ss << "复用成功, 数量: " << elem_cnt << ", 占用空间: " << (file_aligned_size / 1024 / 1024) << " MB";
                log_msg("INFO", ss.str());

                return JoinResult::SUCCESS;
            }

            bool try_create_new(uint64_t requested_count) {
                // O_EXCL 保证原子性：如果文件已存在则报错 EEXIST
                this->shm_fd = shm_open(this->storage_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
                if (this->shm_fd == -1) {
                    return false;
                }

                uint64_t capacity = next_pow2(requested_count);
                auto raw_size = sizeof(ShmHeader) + sizeof(PaddedValue<T, ALIGN>) * capacity;
                uint64_t aligned_sz = align_to_huge_page(raw_size);

                if (ftruncate(this->shm_fd, aligned_sz) == -1) {
                    log_msg("ERROR", "ftruncate 失败: " + string(strerror(errno)));
                    close(this->shm_fd);
                    shm_unlink(this->storage_name.c_str());
                    return false;
                }

                this->mapped_ptr = map_memory_segment(aligned_sz, true);
                if (!this->mapped_ptr) {
                    log_msg("WARN", "创建时 HugePage 失败，降级为普通页");
                    this->mapped_ptr = map_memory_segment(aligned_sz, false);
                    if (!this->mapped_ptr) {
                        close(this->shm_fd);
                        shm_unlink(this->storage_name.c_str());
                        return false;
                    }
                }
                this->mapped_size = aligned_sz;

                // 初始化 Header
                auto header = new (this->mapped_ptr) ShmHeader();
                header->element_count = capacity;
                header->element_size = sizeof(T);
                header->padded_element_size = sizeof(PaddedValue<T, ALIGN>);
                header->aligned_file_size = aligned_sz;
                header->producer_idx.store(0, std::memory_order_relaxed);
                header->consumed_idx.store(0, std::memory_order_relaxed);
                header->futex_flag.store(0, std::memory_order_relaxed);

                // 内存屏障设置魔数
                std::atomic_thread_fence(std::memory_order_release);
                header->magic_num = SHM_READY_MAGIC;

                this->view.init(this->mapped_ptr);

                stringstream ss;
                ss << "创建成功, 请求: " << requested_count << ", 实际容量(2^n): " << capacity
                   << ", 总大小: " << (aligned_sz / 1024 / 1024) << " MB";
                log_msg("INFO", ss.str());
                return true;
            }

        public:
            MemoryStorage() = default;
            // 禁止拷贝
            MemoryStorage(const MemoryStorage&) = delete;
            MemoryStorage& operator=(const MemoryStorage&) = delete;

            bool build(string storage_name, uint64_t requested_count) {
                if (geteuid() != 0) {
                    throw std::runtime_error("致命错误: 需要 Root 权限以使用 HugePage/mmap");
                }
                this->storage_name = storage_name;

                for (auto i = 0; i < 3; i += 1) {
                    // 尝试 Join
                    auto join_res = try_join_existing();

                    if (join_res == JoinResult::SUCCESS) {
                        return true; // Is Join = true
                    }

                    if (join_res == JoinResult::TYPE_MISMATCH) {
                        throw std::runtime_error("共享内存数据结构版本不匹配");
                    }

                    if (join_res == JoinResult::DATA_CORRUPT) {
                        // 已在 try_join 中执行 unlink，直接进入下一次循环尝试 create
                        log_msg("WARN", "检测到损坏的文件，已删除并重试创建...");
                        continue;
                    }

                    // 尝试 Create
                    // join 返回 FILE_NOT_FOUND 或者其他可恢复错误时，尝试创建
                    if (try_create_new(requested_count)) {
                        return false; // Is Join = false (We are creator)
                    }

                    // 并发处理
                    if (errno == EEXIST) {
                        log_msg("WARN", "检测到并发竞争 (EEXIST)，正在重试 join...");
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }

                    // 其他未知错误
                    throw std::runtime_error("shm_open 致命错误: " + string(strerror(errno)));
                }

                throw std::runtime_error("由于严重的并发竞争，初始化超时");
            }

            inline SharedQueueView<T, ALIGN>& get_view() {
                return this->view;
            }

            ~MemoryStorage() {
                if (this->mapped_ptr) {
                    munmap(this->mapped_ptr, this->mapped_size);
                    log_msg("INFO", "内存映射已解除");
                }

                if (shm_fd != -1) {
                    close(shm_fd);
                }
            }
    };
} // namespace mem_queue

#pragma pack(pop)

#endif //MEM_QUEUE_HPP