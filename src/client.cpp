//
// Created by yanguangshaonian on 25-11-17.
//

#include "iostream"
#include "lib.hpp"
#include "my_struct.hpp"
using namespace std;

inline __attribute__((constructor)) void lock_memory() {
    // 锁定当前已分配的内存 和 未来将分配的内存
    printf("constructor lock_memory\n");
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        perror("constructor mlockall failed");
    }
    printf("constructor Successfully locked memory.\n");
}

int main() {
    uint64_t age_cnt = 0;
    uint64_t over_cnt = 0;
    uint64_t cnt = 0;
    auto t = thread([&]() {
        auto store_name = string("student");

        auto memory_store = mem_queue::MemoryStorage<Student, 4>{};
        memory_store.build(store_name, 1024 * 1024 * 2);

        auto& store = memory_store.get_view();
        cnt = store.get_consumed_idx();
        // cnt = store.get_producer_idx();
        cout << "cnt: " << cnt << endl;
        cout << "producer_idx " << store.get_producer_idx() << endl;
        cout << "consumed_idx " << store.get_consumed_idx() << endl;

        while (true) {
            auto read_ret = store.read_wait_competing(cnt, [&](const Student& student, bool is_owner) {
                if (is_owner) {
                    age_cnt += student.age;
                }
            });

            // auto read_ret = store.read_wait(cnt, [&](const Student& student) {
            //     age_cnt += student.age;
            // });

            if (read_ret == mem_queue::ReadStatus::OVERWRITTEN) {
                over_cnt += 1;
                cnt += 1;
            } else if (read_ret == mem_queue::ReadStatus::SUCCESS) {
                cnt += 1;
            }
        }
    });
    t.detach();

    while (true) {
        sleep(1);
        cout << "---" << endl;
        cout << "age_cnt: " << age_cnt << endl;
        cout << "cnt: " << cnt << endl;
        cout << "over_cnt: " << over_cnt << endl;
    }
    cout << "client" << endl;
}