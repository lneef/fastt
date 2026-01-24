#pragma once

#include "message.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

template <uint32_t N> struct window {
  window(uint64_t min_seq)
      : wd(N), front(0), mask(N - 1), least_in_window(min_seq){}

  uint64_t get_last_acked_packet() const { return least_in_window - 1; }

  bool set(uint64_t seq, message *msg) {
    auto i = index(seq);
    if (beyond_window(seq) || wd[i])
      return false;
    if (seq > largest_in_window)
      largest_in_window = seq;
    wd[i] = true;
    messages[i] = msg;
    return true;
  }

  bool is_set(uint64_t seq) {
    return seq < least_in_window ||
           (seq <= least_in_window + mask && wd[index(seq)]);
  }

  bool beyond_window(uint64_t seq) { return seq > least_in_window + mask; }

  uint32_t advance(std::invocable<message *> auto &&f) {
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
    return least_in_window + mask - largest_in_window;
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

  bool has_holes() { return largest_in_window != least_in_window; }

  uint16_t copy_bitset(uint8_t *data) {
    uint16_t id = 0;
    uint8_t mask = sizeof(uint8_t) * 8 - 1;
    std::memset(data, 0, (largest_in_window - least_in_window) / 8 + 1);
    for (auto i = least_in_window; i <= largest_in_window; ++i, ++id) {
      auto bit_idx = id & mask;
      auto idx = id >> 3;
      data[idx] |= wd[index(i)] << bit_idx;
    }
    return id;
  }

  std::size_t last_seq() const { return least_in_window + mask + 1; }

  std::vector<bool> wd;
  std::array<message *, N> messages{};
  std::size_t front, mask;
  uint64_t least_in_window;
  uint64_t largest_in_window;
};
