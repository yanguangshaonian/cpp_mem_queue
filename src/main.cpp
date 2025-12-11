//
// Created by yanguangshaonian on 25-11-17.
//

#include "iostream"
#include "lib.hpp"
#include "my_struct.hpp"

using namespace std;

int main() {
    auto memory_store = MemoryStorage<Student, 1024 * 1024>{};
    auto store_name = string("student");
    auto data_store = memory_store.init(store_name, Role::WRITER);

    sleep(5);
    uint64_t age_cnt = 0;
    cout << "start" << endl;
    auto start_time = steady_clock::now();
    for (int i = 0; i < 1024 * 1024 * 10; ++i) {
        // data_store->write_wake([&](Student& student) {
        data_store->write([&](Student& student) {
            student.age = i;
        });
        age_cnt += i;

        _mm_pause();
    }
    cout << "end" << endl;
    auto end_time = steady_clock::now();

    sleep(3);
    cout << "main " << age_cnt << endl;
    cout << "耗时: " << duration_cast<std::chrono::milliseconds>(end_time - start_time).count() << "ms" << endl;
}