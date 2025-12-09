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
    auto data_store = memory_store.create(store_name);

    sleep(5);
    uint64_t age_cnt = 0;

    for (int i = 0; i < 1024 * 1024 * 10; ++i) {
        data_store->write([&](Student& student) {
            student.age = i;
        });
        age_cnt += i;

        _mm_pause();
        _mm_pause();
    }

    sleep(3);
    cout << "创建完成" << endl;
    cout << "main " << age_cnt << endl;
}