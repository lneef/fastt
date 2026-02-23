#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

struct seq_t {
  uint32_t v;

  friend bool operator<(seq_t a, seq_t b) {
    return std::bit_cast<int32_t>(a.v - b.v) < 0;
  }

  friend bool operator>(seq_t a, seq_t b) { return std::bit_cast<int32_t>(a.v - b.v) > 0; }
  friend bool operator<=(seq_t a, seq_t b) { return std::bit_cast<int32_t>(a.v - b.v) <= 0; }
  friend bool operator>=(seq_t a, seq_t b) { return std::bit_cast<int32_t>(a.v - b.v) >= 0; }
  friend bool operator==(seq_t a, seq_t b) { return a.v == b.v; }
  friend bool operator!=(seq_t a, seq_t b) { return a.v != b.v; }

  friend seq_t operator+(seq_t a, uint32_t n) { return {a.v + n}; }
  friend seq_t operator-(seq_t a, uint32_t n) { return {a.v - n}; }
  friend uint32_t operator-(seq_t a, seq_t b) { return a.v - b.v; }

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

static_assert(std::is_trivially_copyable_v<seq_t>);
static_assert(sizeof(seq_t) == 4);
