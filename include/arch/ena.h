#pragma once

#include <cstdint>
#include <rte_ethdev.h>
#include <vector>
#include <array>
#include <cstring>

#include "nic.h"
namespace ena {
static constexpr unsigned kQueueByteOffset = 9;
static constexpr unsigned kQueueByteStep = 2;

inline constexpr std::array<uint8_t, 40> RSS_DEFAULT_KEY = {
    0xbe, 0xac, 0x01, 0xfa, 0x6a, 0x42, 0xb7, 0x3b,
    0x80, 0x30, 0xf2, 0x0c, 0x77, 0xcb, 0x2d, 0xa3,
    0xae, 0x7b, 0x30, 0xb4, 0xd0, 0xca, 0x2b, 0xcb,
    0x43, 0xa3, 0x8f, 0xb0, 0x41, 0x67, 0x25, 0x3d,
    0x25, 0x5b, 0x0e, 0xc2, 0x6d, 0x5a, 0x56, 0xda
};

inline uint32_t toeplitz_hash(uint32_t src_ip,
                              uint32_t dst_ip,
                              uint16_t src_port,
                              uint16_t dst_port,
                              const std::array<uint8_t, 40>& key = RSS_DEFAULT_KEY,
                              uint32_t initial = 0) {
    std::array<uint8_t, 12> input;
    std::memcpy(&input[0], &src_ip,   4);
    std::memcpy(&input[4], &dst_ip,   4);
    std::memcpy(&input[8], &src_port, 2);
    std::memcpy(&input[10], &dst_port, 2);

    std::array<uint8_t, 40> k = key;
    uint32_t hash = initial;

    for (uint8_t byte : input) {
        for (int i = 0; i < 8; ++i) {
            if (byte & (1 << (7 - i))) {
                hash ^= (static_cast<uint32_t>(k[0]) << 24) |
                        (static_cast<uint32_t>(k[1]) << 16) |
                        (static_cast<uint32_t>(k[2]) << 8)  |
                         static_cast<uint32_t>(k[3]);
            }
            constexpr int L = 40;
            uint8_t carry = (k[0] >> 7) & 1;
            for (int j = 0; j < L - 1; ++j)
                k[j] = (k[j] << 1) | ((k[j + 1] >> 7) & 1);
            k[L - 1] = (k[L - 1] << 1) | carry;
        }
    }

    return hash;
}

struct ena : public nic{
  unsigned best = 0;
  unsigned best_idx = 0;
  std::vector<uint64_t> ids;
  std::vector<uint64_t> values;

  void update(int port) override{
    best = UINT64_MAX;
    best_idx = 0;
    rte_eth_xstats_get_by_id(port, ids.data(), values.data(), ids.size());
    for (unsigned i = 0; i < ids.size(); ++i) {
      if (values[i] < best) {
        best = values[i];
        best_idx = i;
      }
    }
  }

  uint64_t calc_rss_hash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport) override{
      return toeplitz_hash(sip, dip, sport, dport);
  }

  void find_port_pair(uint32_t sip, uint32_t dip, uint16_t& sport, uint16_t &dport, uint16_t rtid) override{
      for(uint16_t s = 0; s < UINT16_MAX; ++s){
          for(uint16_t d = 0; d < UINT16_MAX; ++d){
              auto hash = calc_rss_hash(sip, dip, htons(s), htons(d)); 
              if(hash % rtid == 0){
                  sport = s;
                  dport = d;
                  return;
              }
          }
      }

  }

  unsigned best_queue() const override { return best_idx; }

  ena(unsigned n_qpair) : nic(n_qpair), ids(n_qpair), values(n_qpair) {
    unsigned i = 0;
    for (auto &id : ids)
      id = kQueueByteOffset + i++ * kQueueByteStep;
  }
};

} // namespace ena
