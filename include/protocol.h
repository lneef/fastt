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
    FT_MSG, FT_ACK, FT_INIT
};

struct header_base {
  pkt_type type :2;
  uint64_t wnd :16;
  uint64_t msg_id :46;
};

struct __rte_packed_begin ft_header : header_base {
  uint64_t seq;
  uint64_t ack;
} __rte_packed_end;

struct __rte_packed_begin init_header : header_base {
  uint64_t seq;
} __rte_packed_end;

void prepare_ft_header(message* msg, uint64_t seq, uint64_t ack, uint64_t msg_id, uint16_t wnd);
message* prepare_ack_pkt(uint64_t ack, message_allocator* pool, uint16_t wnd);
void prepare_init_header(message* msg, uint64_t seq);

namespace defs{
  static constexpr uint16_t kipOffset = sizeof(rte_ether_hdr);
  static constexpr uint16_t kudpOffset = kipOffset + sizeof(rte_ipv4_hdr);
  static constexpr uint16_t kdataOffset = kudpOffset + sizeof(rte_udp_hdr);  
  static constexpr uint16_t kuserDataOffset = kdataOffset + sizeof(ft_header);
};

} // namespace protocol
