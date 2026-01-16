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
  FT_INIT,
};

struct __rte_packed_begin ft_header {
  pkt_type type : 2;
  uint32_t wnd : 16;
  uint32_t fini : 1;
  uint32_t reserved : 11;
  uint32_t urgent : 1;
  uint32_t nack : 1;
  uint32_t msg_id;
  uint32_t seq;
  uint32_t ack;
  uint64_t len : 16;
  uint64_t out : 48;
} __rte_packed_end;

void prepare_ft_header(message *msg, uint64_t seq, uint64_t ack,
                       uint32_t msg_id, uint16_t wnd, uint64_t out, bool fin);
message *prepare_ack_header(uint64_t ack, message_allocator *pool, uint16_t wnd, uint32_t msg_id);
message *prepare_init_header(uint64_t seq, message_allocator* alloc, uint16_t wnd);

namespace defs {
static constexpr uint16_t kipOffset = sizeof(rte_ether_hdr);
static constexpr uint16_t kudpOffset = kipOffset + sizeof(rte_ipv4_hdr);
static constexpr uint16_t kftOffset = kudpOffset + sizeof(rte_udp_hdr);
static constexpr uint16_t kuserDataOffset = kftOffset + sizeof(ft_header);
}; // namespace defs

} // namespace protocol
