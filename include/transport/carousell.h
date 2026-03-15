#pragma once

#include "util.h"
#include "slab_allocator.h"
#include <bit>
#include <cstdint>
#include <deque>

template<typename T>
class carousell {
  struct packet_desc_t {
    transport_config *cfg;
    mbuf *pkt;
  };
  struct wheel_element_t {
    std::deque<packet_desc_t> pkts;
  };

public:
  carousell(uint64_t granularity, uint64_t horizon)
      : front_timestamp(rte_get_timer_cycles() / granularity),
        granularity(granularity), slots(std::bit_ceil(horizon / granularity)),
        wheel(slots) {}

  void insert(transport_config *cfg, mbuf *pkt,
              uint64_t ts) {
    ts = (ts) / granularity;
    if (ts <= front_timestamp)
      ts = front_timestamp;
    else if (ts > front_timestamp + (slots - 1))
      ts = front_timestamp + (slots - 1);
    wheel[ts & (slots - 1)].emplace_back(cfg, pkt);
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
  std::vector<std::deque<packet_desc_t>> wheel;
};
