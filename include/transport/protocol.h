#pragma once

#include "slab_allocator.h"
#include "transport/seq.h"
#include "util.h"
#include <cstdint>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_mempool.h>
#include <utility>

namespace protocol {
enum pkt_type : uint8_t {
  FT_MSG = 0,
  FT_ACK = 1,
  FT_SYN = 2,
  FT_SYN_ACK = 3,
  FT_WND_RET = 4,
  FT_DONE = 5,
};

struct [[gnu::packed]] ft_header {
  uint16_t sport;
  uint16_t dport;
  seq_t seq;
  seq_t ack;
  pkt_type type : 3;
  uint32_t ackframe : 1;
  uint32_t sack : 1;
  uint32_t som : 1;
  uint32_t eom : 1;
  uint32_t wnd: 25;
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

struct [[gnu::packed]] ft_msg_payload{
    uint64_t out;
};

inline void extract_ports(flow_tuple &ft, mbuf *pkt) {
  auto *hdr = pkt->data<protocol::ft_header>();
  ft.sport = hdr->sport;
  ft.dport = hdr->dport;
}

struct msg_frame_desc {
  seq_t seq, ack;
  uint16_t wnd;
  bool som, eom;
  bool ack_frame, sack;
};

struct builder {
  uint16_t sport, dport;
  builder(uint16_t sport, uint16_t dport) : sport(sport), dport(dport) {}
  inline void prepare_ft_header(mbuf *msg, const msg_frame_desc &desc) {
    auto *ft = msg->prepend<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->ack = desc.ack;
    ft->seq = desc.seq;
    ft->wnd = desc.wnd;
    ft->ackframe = desc.ack_frame;
    ft->sack = desc.sack;
    ft->som = desc.som;
    ft->eom = desc.eom;
    ft->type = protocol::pkt_type::FT_MSG;
  }

  inline void prepare_ack_pkt(mbuf *msg, seq_t ack,
                              bool is_sack) {
    auto *ft = msg->data<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->ack = ack;
    ft->sack = is_sack;
    ft->wnd = 0;
    ft->type = protocol::pkt_type::FT_ACK;
  }

  inline void prepare_init_header(mbuf *msg, seq_t seq, uint16_t budget) {
    auto *ft = msg->data<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->seq = seq;
    ft->ackframe = 0;
    ft->sack = 0;
    ft->wnd = budget;
    ft->type = protocol::pkt_type::FT_SYN;
  }

  inline void prepare_ctrl_pkt(mbuf *msg, seq_t seq, seq_t ack, uint16_t wnd,
                               bool is_ack_frame) {
    auto *ft = msg->data<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->seq = seq;
    ft->ack = ack;
    ft->wnd = wnd;
    ft->ackframe = is_ack_frame;
    ft->type = protocol::pkt_type::FT_WND_RET;
  }

  inline void prepare_init_ack_header(mbuf *msg, seq_t seq, seq_t ack,
                                      uint16_t wnd, bool is_ack_frame) {
    auto *ft = msg->data<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->ack = ack;
    ft->wnd = wnd;
    ft->seq = seq;
    ft->ackframe = is_ack_frame;
    ft->sack = 0;
    ft->type = protocol::pkt_type::FT_SYN_ACK;
  }

  inline void prepare_done_header(mbuf *msg, seq_t seq, seq_t ack, bool is_ack_frame) {
    auto *ft = msg->data<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->seq = seq;
    ft->ack = ack;
    ft->ackframe = is_ack_frame;
    ft->type = protocol::pkt_type::FT_DONE;
  }
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
