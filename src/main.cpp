//
// Created by yanguangshaonian on 25-11-17.
//

#include "iostream"
#include "lib.hpp"
#include "my_struct.hpp"
#include <chrono>
#include <cstdint>

using namespace std;
using namespace std::chrono;

// MemoryStorage<Student, 1024 * 1024>::StoreType* store;

int main() {
    auto cnt = 1024 * 1024;
    auto store_name = string("student");

    auto memory_store = mem_queue::MemoryStorage<Student>{};
    memory_store.build(store_name, cnt);

    // auto& store = memory_store.get_store();
    auto store = memory_store.get_view();
    sleep(1);
    uint64_t age_cnt = 0;
    cout << "start" << endl;
    auto start_time = steady_clock::now();

    for (uint64_t i = 0; i < 1024ULL * 1024 * 10; ++i) {
        // memory_store.layout_ptr->store.write([&](Student& student) {
        store.write([&](Student& student) {
            // data_store->write_wake([&](Student& student) {
            student.age = i;
        });
        age_cnt += i;

        _mm_pause();
    }
    cout << "end" << endl;
    auto end_time = steady_clock::now();

    sleep(1);
    cout << "main " << age_cnt << endl;
    cout << "耗时: " << duration_cast<std::chrono::milliseconds>(end_time - start_time).count() << "ms" << endl;
}