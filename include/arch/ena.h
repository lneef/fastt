#pragma once

#include "nic.h"
#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <cstring>
namespace ena {
static constexpr unsigned kQueueByteOffset = 9;
static constexpr unsigned kQueueByteStep = 2;
static constexpr unsigned kBitsinBytes = 8;
static constexpr unsigned kENAKeyLen = 40;

inline constexpr std::array<uint8_t, kENAKeyLen> RSS_DEFAULT_KEY = {
    0xbe, 0xac, 0x01, 0xfa, 0x6a, 0x42, 0xb7, 0x3b, 0x80, 0x30,
    0xf2, 0x0c, 0x77, 0xcb, 0x2d, 0xa3, 0xae, 0x7b, 0x30, 0xb4,
    0xd0, 0xca, 0x2b, 0xcb, 0x43, 0xa3, 0x8f, 0xb0, 0x41, 0x67,
    0x25, 0x3d, 0x25, 0x5b, 0x0e, 0xc2, 0x6d, 0x5a, 0x56, 0xda};

/*
 * Adapted from
 * https://github.com/amzn/amzn-ec2-ena-utilities/blob/main/ena-toeplitz/toeplitz_calc.py
 */
inline uint32_t
toeplitz_hash(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
              uint16_t dst_port,
              const std::array<uint8_t, kENAKeyLen> &key = RSS_DEFAULT_KEY,
              uint32_t initial = 0) {
  static constexpr unsigned kInputLen = 12;
  std::array<uint8_t, kInputLen> input;
  std::memcpy(&input[0], &src_ip, 4);
  std::memcpy(&input[4], &dst_ip, 4);
  std::memcpy(&input[8], &src_port, 2);
  std::memcpy(&input[10], &dst_port, 2);

  std::array<uint8_t, kENAKeyLen> k = key;
  uint32_t hash = initial;

  for (uint8_t byte : input) {
    for (int i = 0; i < 8; ++i) {
      if (byte & (1 << (kBitsinBytes - i - 1)))
        hash ^= (static_cast<uint32_t>(k[0]) << 24) |
                (static_cast<uint32_t>(k[1]) << 16) |
                (static_cast<uint32_t>(k[2]) << 8) |
                static_cast<uint32_t>(k[3]);

      for (unsigned j = 0; j < kENAKeyLen; ++j)
        k[j] = ((k[j] << 1) & 0xff) | ((k[(j + 1) % kENAKeyLen] & 0x80) >> 7);
    }
  }

  return hash;
}

struct ena : public nic {
  static constexpr uint16_t kRetaSize = 128;
  uint64_t calc_rss_hash(uint32_t sip, uint32_t dip, uint16_t sport,
                         uint16_t dport) override {
    return toeplitz_hash(sip, dip, sport, dport);
  }

  void find_port_pair(uint32_t sip, uint32_t dip, uint16_t &sport,
                      uint16_t &dport, uint16_t rtid, uint16_t cores) override {
    for (uint16_t s = 0; s < UINT16_MAX; ++s) {
      for (uint16_t d = 0; d < UINT16_MAX; ++d) {
        auto hash = calc_rss_hash(sip, dip, htons(s), htons(d));
        if ((hash % kRetaSize) % cores == rtid) {
          sport = htons(s);
          dport = htons(d);
          return;
        }
      }
    }
  }

  void find_one(uint32_t sip, uint32_t dip, uint16_t &sport, uint16_t dport,
                uint16_t rtid, uint16_t cores) {
    for (uint16_t s = 0; s < UINT16_MAX; ++s) {
      auto hash = calc_rss_hash(sip, dip, htons(s), dport);
      if ((hash % kRetaSize) % cores == rtid) {
        sport = htons(s);
        return;
      }
    }
  }

  ~ena() override = default;
};

} // namespace ena
