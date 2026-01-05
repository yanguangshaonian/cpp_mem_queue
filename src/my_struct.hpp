//
// Created by yanguangshaonian on 25-11-17.
//

#ifndef MY_STRUCT_HPP
#define MY_STRUCT_HPP

class Student {
    public:
        char name[3]{};
        uint64_t age;

        Student() = default;

        explicit Student(const uint64_t age)
            : age(age) {
            name[0] = '1';
        }
};


#endif //MY_STRUCT_HPP
