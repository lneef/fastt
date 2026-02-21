#pragma once

#include <bit>
#include <cstdint>

struct seq_num{
    uint32_t num;
};

__inline bool operator<(const seq_num a, const seq_num b){
    return std::bit_cast<int32_t>(a.num - b.num) < 0;
}

__inline bool operator>(const seq_num a, const seq_num b){
    return std::bit_cast<int32_t>(a.num - b.num) > 0;
}

__inline bool operator<=(const seq_num a, const seq_num b){
    return !(a > b);
}

__inline bool operator>=(const seq_num a, const seq_num b){
    return !(a < b);
}

__inline bool operator==(const seq_num a, const seq_num b){
    return a.num == b.num;
}

__inline bool operator!=(const seq_num a, const seq_num b){
    return a.num != b.num;
}

__inline seq_num operator+(const seq_num a, const uint32_t n){
    return {a.num + n};
}

__inline seq_num operator-(const seq_num a, const seq_num b){
    return {a.num - b.num};
}
