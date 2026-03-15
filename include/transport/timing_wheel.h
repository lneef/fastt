#pragma once

#include "util.h"
#include <bit>
#include <cstdint>

template<typename T>
class timing_wheel {
public:
  timing_wheel(uint64_t granularity, uint64_t horizon)
      : front_timestamp(rte_get_timer_cycles() / granularity),
        granularity(granularity), slots(std::bit_ceil(horizon / granularity)),
        wheel(slots) {}

  void insert(typename T::value_type& elem, uint64_t ts) {
    ts = (ts) / granularity;
    if (ts <= front_timestamp)
      ts = front_timestamp;
    else if (ts > front_timestamp + (slots - 1))
      ts = front_timestamp + (slots - 1);
    wheel[ts & (slots - 1)].push_back(elem);
  }

  bool beyond_horizon(uint64_t ts) const{
      return ts > (front_timestamp + slots - 1) * granularity;
  }

  template <typename F> void advance(uint64_t now, F &&consumer) {
    now /= granularity;
    while (now >= front_timestamp) {
      auto &entry = wheel[now & (slots - 1)];
      while (!entry.empty()) {
        auto &p_desc = entry.front();
        consumer(p_desc.cfg, p_desc.pkt);
        entry.pop_front();
        ++front_timestamp;
      }
    }
  }

private:
  uint64_t front_timestamp, granularity;
  uint32_t slots;
  std::vector<T> wheel;
};
