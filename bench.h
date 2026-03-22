#pragma once

#include "tlx/container/btree_map.hpp"
#include <cstdint>
#include <random>
#include <ranges>

namespace bench {
using req_desc_t = std::pair<uint64_t, int64_t>;
using storage = tlx::btree_map<int64_t, std::string>;

static constexpr size_t kStoreSize = 1024 * 1024;
static constexpr size_t kMaxBurstLimit = 32;


inline std::string random_string(std::size_t length) {
    static constexpr char chars[] =
        "a";

    static std::mt19937 rng{std::random_device{}()};
    static std::uniform_int_distribution<std::size_t> dist(0, sizeof(chars) - 2);

    std::string s(length, '\0');
    for (auto& c : s) c = chars[dist(rng)];
    return s;
}

inline void prepare(storage& store, size_t sz) {
  uint32_t size = kStoreSize;
  for (auto [k, v] :
       std::ranges::views::iota(0u, size) | std::views::transform([&](size_t k) {
         return std::make_pair(k, random_string(sz));
       })) {
    store[k] = v;
  }
}

};
