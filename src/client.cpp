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

    auto cnt = 0;
    uint64_t age_cnt = 0;
    uint64_t over_cnt = 0;

    auto t = thread([&]() {
        auto memory_store = mem_queue::MemoryStorage<Student, 1024 * 1024>{};
        auto store_name = string("student");

        memory_store.build(store_name, mem_queue::Role::READER);
        auto& store = memory_store.get_store();
        cnt = store.get_producer_idx();


        while (true) {
            // auto read_ret = data_store->read_wait(cnt, [&](const Student& student) {
            auto read_ret = store.read(cnt, [&](const Student& student) {
                // auto read_ret = store.read_competing(cnt, [&](const Student& student, bool is_owner) {
                // auto read_ret = data_store->read_umwait(cnt, [&](const Student& student) {

                age_cnt += student.age;
            });
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