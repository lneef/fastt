#pragma once
#include <cstdint>
#include <utility>

namespace filter{

static constexpr uint64_t w1 = 1, w2 = 15, shift = 4;

template<typename T>
__inline T exp_filter(T val, T measured){
    return  (w1 * measured + w2 * val) >> shift;
}

static __inline std::pair<uint64_t, uint64_t>
estimate_exp(uint64_t rtt, uint64_t rtt_dv, uint64_t measured) {
  static constexpr uint64_t w1 = 1, w2 = 7, shift = 3;
  auto nrtt = (w1 * measured + w2 * rtt) >> shift;
  auto diff = measured > rtt ? measured - rtt : rtt - measured;
  auto nrtt_dv = (w1 * diff + w2 * rtt_dv) >> shift;
  return {nrtt, nrtt_dv};
}
}
