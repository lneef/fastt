#pragma once

#include "message.h"
#include <cstdint>
#include <rte_common.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mempool.h>
#include <rte_udp.h>

namespace protocol {
enum pkt_type : uint8_t {
  FT_MSG = 0,
  FT_ACK = 1,
  FT_INIT = 2,
  FT_INIT_ACK = 3
};

struct __rte_packed_begin ft_header {
  pkt_type type : 3;
  uint64_t wnd : 26;
  uint64_t start :1;
  uint64_t end : 1;
  uint64_t sack : 1;
  uint64_t ts : 32;
  uint64_t seq;
  uint64_t ack;
} __rte_packed_end;

static_assert(sizeof(ft_header) == 24, "");

struct __rte_packed_begin ft_sack_payload {
  static constexpr uint16_t kBitMapLen = 4;
  uint64_t bit_map[kBitMapLen];
  uint16_t bit_map_len;
} __rte_packed_end;

void prepare_ft_header(message *msg, uint64_t seq, uint64_t ack,  uint16_t wnd, 
                       bool start, bool fini, uint32_t us = 0);
void prepare_ack_pkt(message *msg, uint64_t ack, uint16_t wnd, uint32_t us,
                     bool is_sack = false);
void prepare_init_header(message *msg, uint64_t seq);
void prepare_init_ack_header(message *msg, uint64_t seq, uint64_t ack,
                             uint16_t wnd);

namespace defs {
static constexpr uint16_t kipOffset = sizeof(rte_ether_hdr);
static constexpr uint16_t kudpOffset = kipOffset + sizeof(rte_ipv4_hdr);
static constexpr uint16_t kftOffset = kudpOffset + sizeof(rte_udp_hdr);
static constexpr uint16_t kuserDataOffset = kftOffset + sizeof(ft_header);

static constexpr uint16_t kL2len = sizeof(rte_ether_hdr);
static constexpr uint16_t kL3len = sizeof(rte_ipv4_hdr);
static constexpr uint16_t kL4len = sizeof(rte_udp_hdr);
}; // namespace defs

} // namespace protocol
