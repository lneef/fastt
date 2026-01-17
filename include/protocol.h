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
  uint32_t wnd :16;
  uint32_t fini: 1;
  uint32_t reserved: 13;
  uint32_t msg_id;  
  uint64_t seq;
  uint64_t ack;
} __rte_packed_end;


void prepare_ft_header(message* msg, uint64_t seq, uint64_t ack, uint64_t msg_id, uint16_t wnd);
message* prepare_ack_pkt(uint64_t ack, message_allocator* pool, uint16_t wnd);
message* prepare_init_header(message_allocator* allocator, uint64_t seq);
message* prepare_init_ack_header(message_allocator* pool, uint64_t seq, uint64_t ack, uint16_t wnd);

namespace defs{
  static constexpr uint16_t kipOffset = sizeof(rte_ether_hdr);
  static constexpr uint16_t kudpOffset = kipOffset + sizeof(rte_ipv4_hdr);
  static constexpr uint16_t kftOffset = kudpOffset + sizeof(rte_udp_hdr);  
  static constexpr uint16_t kuserDataOffset = kftOffset + sizeof(ft_header);
};

} // namespace protocol
