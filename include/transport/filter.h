#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>

namespace filter{

static constexpr uint64_t w1_def = 1, w2_def = 7, shift_def = 3;

template<typename T, uint64_t w1 = w1_def, uint64_t w2 = w2_def, uint64_t shift = shift_def>
__inline T exp_filter(T val, T measured){
    return  (w1 * measured + w2 * val) >> shift;
}

template<typename T>
__inline T min_filter(T val, T measured){
    return std::min<T>(val, measured);
}

template<uint64_t w1 = w1_def, uint64_t w2 = w2_def, uint64_t shift = shift_def>
static __inline std::pair<uint64_t, uint64_t>
estimate_exp(uint64_t rtt, uint64_t rtt_dv, uint64_t measured) {
  auto nrtt = (w1 * measured + w2 * rtt) >> shift;
  auto diff = measured > rtt ? measured - rtt : rtt - measured;
  auto nrtt_dv = (w1 * diff + w2 * rtt_dv) >> shift;
  return {nrtt, nrtt_dv};
}

}
