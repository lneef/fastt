#pragma once

#include "message.h"
#include <cstdint>
#include <rte_common.h>
#include <rte_ip.h>
#include <rte_ether.h>
#include <rte_mempool.h>
#include <rte_udp.h>

namespace protocol {
enum pkt_type : uint8_t{
    FT_MSG = 0, FT_ACK = 1, FT_INIT = 2, FT_INIT_ACK = 3
};

struct __rte_packed_begin ft_header{
  pkt_type type :2;
  uint64_t wnd :16;
  uint64_t fini: 1;
  uint64_t sack: 1;
  uint64_t msg_id : 14;  
  uint64_t ts : 30;
  uint64_t seq;
  uint64_t ack;
} __rte_packed_end;

static_assert(sizeof(ft_header) == 24, "");

struct __rte_packed_begin ft_sack_payload{
    static constexpr uint16_t kBitMapLen = 2;
    uint64_t bit_map[kBitMapLen];
    uint16_t bit_map_len;
}__rte_packed_end;


void prepare_ft_header(message* msg, uint64_t seq, uint64_t ack, uint64_t msg_id, uint16_t wnd, bool fini = false, uint32_t us = 0);
void prepare_ack_pkt(message* msg, uint64_t ack, uint16_t wnd, uint32_t us, bool is_sack = false);
void prepare_init_header(message* msg, uint64_t seq);
void prepare_init_ack_header(message* msg, uint64_t seq, uint64_t ack, uint16_t wnd);

namespace defs{
  static constexpr uint16_t kipOffset = sizeof(rte_ether_hdr);
  static constexpr uint16_t kudpOffset = kipOffset + sizeof(rte_ipv4_hdr);
  static constexpr uint16_t kftOffset = kudpOffset + sizeof(rte_udp_hdr);  
  static constexpr uint16_t kuserDataOffset = kftOffset + sizeof(ft_header);
};

} // namespace protocol
