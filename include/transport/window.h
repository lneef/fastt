#pragma once

#include "message.h"
#include "protocol.h"
#include "util.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <generic/rte_cycles.h>

template <uint32_t N> struct window {
  window(uint64_t min_seq)
      : wd(), front(0), mask(N - 1), least_in_window(min_seq), max_acked(0){}

  uint64_t get_last_acked_packet() const { return least_in_window - 1; }

  bool set(uint64_t seq, message *msg) {
    auto i = index(seq);
    if (beyond_window(seq) || wd[i])
      return false;
    if (seq > max_acked){
      max_acked = seq;
      ts = *msg->get_ts();
    }
    wd[i] = true;
    messages[i] = msg;
    return true;
  }

  bool is_set(uint64_t seq) {
    return seq < least_in_window ||
           (seq <= least_in_window + mask && wd[index(seq)]);
  }

  bool beyond_window(uint64_t seq) { return seq > least_in_window + mask; }

  template<typename F>
  uint32_t advance(F &&f) {
    assert(mask + 1 == wd.size());
    uint32_t advanced = 0;
    while (wd[front]) {
      f(messages[front]);
      wd[front] = false;
      front = (front + 1) & mask;
      ++least_in_window;
      ++advanced;
    }
    return advanced;
  }

  bool inside(uint64_t seq) {
    return seq >= least_in_window && seq <= least_in_window + mask;
  }

  uint32_t capacity() const {
    return least_in_window + mask - max_acked;
  }

  std::size_t __inline index(std::size_t i) {
    assert(i >= least_in_window);
    return (i - least_in_window + front) & mask;
  }

  bool try_reserve(uint64_t seq) {
    assert(seq >= least_in_window);
    seq -= least_in_window;
    return seq <= mask;
  }

  bool has_holes() { return max_acked != least_in_window; }

  uint16_t copy_bitset(protocol::ft_sack_payload *data) {  
    uint16_t id = 0;
    std::memset(data, 0, (max_acked - least_in_window + 64) / 64); /* 64 since least_in_window is part of the window */
    for (auto i = least_in_window; i <= max_acked; ++i, ++id) {
      auto ind = get_bit_indices_64(id);  
      data->bit_map[ind.first] |= static_cast<uint64_t>(wd[index(i)]) << ind.second;
    }
    return id;
  }

  std::size_t last_seq() const { return least_in_window + mask + 1; }

  uint64_t get_ts(){
      auto now = rte_get_timer_cycles() / get_ticks_us();
      return now - ts;
  }

  std::bitset<N> wd;
  std::array<message *, N> messages{};
  std::size_t front, mask;
  uint64_t least_in_window;
  uint64_t max_acked;
  uint64_t ts = 0;
};
