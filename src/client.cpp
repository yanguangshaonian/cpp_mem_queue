//
// Created by yanguangshaonian on 25-11-17.
//

#include "iostream"
#include "lib.hpp"
#include "my_struct.hpp"
using namespace std;

int main() {

    auto cnt = 0;
    uint64_t age_cnt = 0;
    uint64_t over_cnt = 0;

    auto t = thread([&]() {
        auto memory_store = MemoryStorage<Student, 1024 * 1024>{};
        auto store_name = string("student");

        memory_store.build(store_name, Role::READER);
        auto& store = memory_store.get_store();
        cnt = store.get_current_idx();


        while (true) {
            // auto read_ret = data_store->read_wait(cnt, [&](const Student& student) {
            auto read_ret = store.read(cnt, [&](const Student& student) {
                // auto read_ret = data_store->read_umwait(cnt, [&](const Student& student) {
                age_cnt += student.age;
            });
            if (read_ret == ReadStatus::OVERWRITTEN) {
                over_cnt += 1;
                cnt += 1;
            } else if (read_ret == ReadStatus::SUCCESS) {
                cnt += 1;
            }
        }
    });
    t.detach();

    while (true) {
        sleep(1);
        cout << "---" << endl;
        cout << age_cnt << endl;
        cout << cnt << endl;
        cout << over_cnt << endl;
    }
    cout << "client" << endl;
}