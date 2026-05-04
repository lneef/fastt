#pragma once

#include <bit>
#include <cstdint>
/*
 * Adapted from
 * https://elixir.bootlin.com/linux/v6.14/source/include/net/tcp.h#L288
 */
struct seq_t {
  uint32_t v;

  seq_t &operator++() {
    ++v;
    return *this;
  }
  seq_t operator++(int) {
    auto t = *this;
    ++v;
    return t;
  }
};

inline bool operator<(seq_t a, seq_t b) {
  return std::bit_cast<int32_t>(a.v - b.v) < 0;
}
inline bool operator>(seq_t a, seq_t b) {
  return std::bit_cast<int32_t>(a.v - b.v) > 0;
}
inline bool operator<=(seq_t a, seq_t b) {
  return std::bit_cast<int32_t>(a.v - b.v) <= 0;
}
inline bool operator>=(seq_t a, seq_t b) {
  return std::bit_cast<int32_t>(a.v - b.v) >= 0;
}
inline bool operator==(seq_t a, seq_t b) { return a.v == b.v; }
inline bool operator!=(seq_t a, seq_t b) { return a.v != b.v; }

inline seq_t operator+(seq_t a, uint32_t n) { return {a.v + n}; }
inline seq_t operator-(seq_t a, uint32_t n) { return {a.v - n}; }
inline uint32_t operator-(seq_t a, seq_t b) { return a.v - b.v; }

static_assert(std::is_trivially_copyable_v<seq_t>);
static_assert(sizeof(seq_t) == 4);
