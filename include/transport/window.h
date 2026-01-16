#pragma once

#include "message.h"

#include <concepts>
#include <limits>
#include <cstdint>
#include <array>

template<auto kMaxOustandingPackets>
struct window {
  window(uint64_t min_seq)
      : front(0), mask(kMaxOustandingPackets - 1), least_in_window(min_seq),
        rtt_est(std::numeric_limits<uint64_t>::max()) {}

  uint64_t get_last_acked_packet() const { return least_in_window - 1; }

  bool stalled(){ return !wd[front] && wd[(front + 1) & mask]; }

  bool set(uint64_t seq, message *msg) {
    auto i = index(seq);
    if (beyond_window(seq) || wd[i])
      return false;
    if (seq > max_acked)
      max_acked = seq;
    wd[i] = true;
    messages[i] = msg;
    return true;
  }

  bool is_set(uint64_t seq) {
    return seq < least_in_window ||
           (seq <= least_in_window + mask && wd[index(seq)]);
  }

  bool beyond_window(uint64_t seq) { return seq > least_in_window + mask; }

  uint64_t advance(std::invocable<message*> auto&&... f) {
    assert(mask + 1 == wd.size());
    while (wd[front]) {
      if ((least_in_window & mask) == 0) {
        last_round = round;
        round = rte_get_timer_cycles();
        did_resize_in_round = false;
        estimate_rtt();
      }
      wd[front] = false;
      (f(messages[front]), ...);
      front = (front + 1) & mask;
      ++least_in_window;
    }
    return least_in_window - 1;
  }

  bool inside(uint64_t seq) {
    return seq >= least_in_window && seq <= least_in_window + mask;
  }

  uint32_t capacity() const { return least_in_window + mask - max_acked; }

  std::size_t __inline index(std::size_t i) {
    assert(i >= least_in_window);
    return (i - least_in_window + front) & mask;
  }

  bool try_reserve(uint64_t seq) {
    assert(seq >= least_in_window);
    seq -= least_in_window;
    return seq <= mask;
  }

  void estimate_rtt() { rtt_est = std::min(rtt_est, round - last_round); }

  uint64_t get_rtt() const { return rtt_est; }

  std::size_t last_seq() const { return least_in_window + mask + 1; }

  std::array<bool, kMaxOustandingPackets> wd{};
  std::array<message *, kMaxOustandingPackets> messages{};
  std::size_t front, mask;
  uint64_t least_in_window;
  uint64_t max_acked = 0;
  uint64_t rtt_est;
  std::size_t acked_in_round = 0;
  uint64_t round = 0, last_round = 0;
  bool did_resize_in_round = true;
};

