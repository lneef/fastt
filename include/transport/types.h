#pragma once

#include <bit>
#include <cstdint>

struct seq_num{
    uint32_t num;

    constexpr seq_num(uint32_t n = 0): num(n){}

    __inline bool operator<(const seq_num other) const{
        return std::bit_cast<int32_t>(num - other.num) < 0;
    }

    __inline bool operator>(const seq_num other) const {
        return std::bit_cast<int32_t>(num - other.num) > 0;
    }

    __inline bool operator<=(const seq_num other) const {
        return !(operator>(other));
    }

    __inline bool operator>=(const seq_num other) const {
        return !(operator<(other));
    }

    __inline bool operator==(const seq_num other) const {
        return num == other.num;
    }

    __inline bool operator!=(const seq_num other) const {
        return num != other.num;
    }

    __inline seq_num operator+(const uint32_t n) const{
        return seq_num(num + n);
    }

    __inline seq_num& operator+=(const uint32_t n){
        num += n;
        return *this;
    }

    __inline seq_num operator++(){
        ++num;
        return *this;
    }

    __inline seq_num operator++(int){
        seq_num old = *this;
        ++num;
        return old;
    }

    __inline seq_num operator-(const seq_num other) const{
        return seq_num(num - other.num);
    }

    static __inline uint32_t to_uint32_t(seq_num sn){
        return sn.num;
    }
};
