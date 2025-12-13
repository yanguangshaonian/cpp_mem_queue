//
// Created by yanguangshaonian on 25-11-17.
//

#include "iostream"
#include "lib.hpp"
#include "my_struct.hpp"

using namespace std;
auto memory_store = MemoryStorage<Student, 1024 * 1024>{};
MemoryStorage<Student, 1024 * 1024>::StoreType* sotre;

int main() {

    auto store_name = string("student");
    memory_store.build(store_name, Role::WRITER);

    // auto& sotre = memory_store.get_store();
    sotre = &memory_store.get_store();
    sleep(1);
    uint64_t age_cnt = 0;
    cout << "start" << endl;
    auto start_time = steady_clock::now();
    for (int i = 0; i < 1024 * 1024 * 10; ++i) {
        // memory_store.layout_ptr->store.write([&](Student& student) {
        memory_store.get_store().write([&](Student& student) {
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