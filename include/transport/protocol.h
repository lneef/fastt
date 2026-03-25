#pragma once

#include "transport/seq.h"
#include <cstdint>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <utility>

namespace protocol {
enum pkt_type : uint8_t {
  FT_MSG = 0,
  FT_ACK = 1,
  FT_SYN = 2,
  FT_SYN_ACK = 3,
  FT_CRD_UPDATE = 4,
  FT_DONE = 5,
};

struct [[gnu::packed]] ft_header {
  uint16_t sport;
  uint16_t dport;
  seq_t seq;
  seq_t ack;
  pkt_type type : 4;
  uint32_t ackframe : 1;
  uint32_t sack : 1;
  uint32_t magic :8;
  uint32_t crd: 18;
};

static_assert(sizeof(ft_header) == 16, "");

struct [[gnu::packed]] ft_sack_payload {
  using interval = std::pair<uint64_t, uint64_t>;
  static constexpr uint16_t kBitMapLen = 4;
  static constexpr uint16_t kMaxIntervalCnt = 64;
  uint64_t bit_map[kBitMapLen];
  uint16_t bit_map_len;
};

struct [[gnu::packed]] ft_init_payload{
    uint16_t sport, dport;
};

namespace defs {
static constexpr uint16_t kipOffset = sizeof(rte_ether_hdr);
static constexpr uint16_t kudpOffset = kipOffset + sizeof(rte_ipv4_hdr);
static constexpr uint16_t kftOffset = kudpOffset + sizeof(rte_udp_hdr);
static constexpr uint16_t kuserDataOffset = kftOffset + sizeof(ft_header);

static constexpr uint16_t kL2len = sizeof(rte_ether_hdr);
static constexpr uint16_t kL3len = sizeof(rte_ipv4_hdr);
static constexpr uint16_t kL4len = sizeof(rte_udp_hdr);
static constexpr uint16_t kFTlen = sizeof(ft_header);
static constexpr uint16_t kHeaderMTUlen = kL3len + kL4len + kFTlen;
}; // namespace defs

} // namespace protocol
