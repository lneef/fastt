#pragma once

#include "message.h"
#include "transport/seq.h"
#include "util.h"
#include <cstdint>
#include <rte_common.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mempool.h>
#include <rte_udp.h>
#include <utility>

namespace protocol {
enum pkt_type : uint8_t {
  FT_MSG = 0,
  FT_ACK = 1,
  FT_RDY_TO_RCV = 2,
  FT_CLR_TO_SD = 3,
  FT_WND_RET = 4,
  FT_DONE = 5,
};

struct __rte_packed_begin ft_header {
  uint16_t sport;
  uint16_t dport;
  pkt_type type : 3;
  uint32_t ackframe :1;
  uint32_t start :1;
  uint32_t end :1;
  uint32_t sack :1;
  uint32_t ts :25;
  uint32_t len: 16;
  uint32_t wnd: 16;
  seq_t seq;
  seq_t ack;
} __rte_packed_end;

static_assert(sizeof(ft_header) == 20, "");

struct __rte_packed_begin ft_sack_payload {
  using interval = std::pair<uint64_t, uint64_t>;  
  static constexpr uint16_t kBitMapLen = 4;
  static constexpr uint16_t kMaxIntervalCnt = 64;
  uint64_t bit_map[kBitMapLen];
  uint16_t bit_map_len;
} __rte_packed_end;

inline void extract_ports(flow_tuple& ft, message* pkt){
    auto *hdr = pkt->data<protocol::ft_header>();
    ft.sport = hdr->sport;
    ft.dport = hdr->dport;
}

struct builder {
  uint16_t sport, dport;
  builder(uint16_t sport, uint16_t dport) : sport(sport), dport(dport) {}
  inline void prepare_ft_header(message *msg, seq_t seq, seq_t ack, uint16_t wnd,
                         bool start, bool end, uint32_t us, bool is_ack_frame, bool is_sack) {
    auto *ft = msg->move_headroom<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->ack = ack;
    ft->seq = seq;
    ft->wnd = wnd;
    ft->start = start;
    ft->end = end;
    ft->ts = us;
    ft->ackframe = is_ack_frame; 
    ft->sack = is_sack;
    ft->type = protocol::pkt_type::FT_MSG;
  }

  inline void prepare_ack_pkt(message *msg, seq_t ack, uint32_t us, bool is_sack) {
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    ft->sport = sport;
    ft->dport = dport;
    ft->ack = ack;
    ft->sack = is_sack;
    ft->wnd = 0;
    ft->ts = us;
    ft->type = protocol::pkt_type::FT_ACK;
  }

  inline void prepare_init_header(message *msg, seq_t seq, uint16_t budget) {
    auto *ft = static_cast<ft_header *>(msg->data());
    ft->sport = sport;
    ft->dport = dport;
    ft->seq = seq;
    ft->ts = 0;
    ft->sack = 0;
    ft->wnd = budget;
    ft->start = true;
    ft->end = true;
    ft->type = protocol::pkt_type::FT_RDY_TO_RCV;
  }

  inline void prepare_ctrl_pkt(message *msg, seq_t seq, uint16_t wnd) {
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    ft->sport = sport;
    ft->dport = dport;
    ft->seq = seq;
    ft->wnd = wnd;
    ft->type = protocol::pkt_type::FT_WND_RET;
  }

  inline void prepare_init_ack_header(message *msg, seq_t seq, seq_t ack,
                               uint16_t wnd) {
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    ft->sport = sport;
    ft->dport = dport;
    ft->ack = ack;
    ft->wnd = wnd;
    ft->seq = seq;
    ft->ackframe = true;
    ft->ts = 0;
    ft->sack = 0;
    ft->start = true;
    ft->end = true;
    ft->type = protocol::pkt_type::FT_CLR_TO_SD;
  }

  inline void prepare_done_header(message *msg, seq_t seq, seq_t ack){
      auto *ft = msg->data<protocol::ft_header>();
      ft->sport = sport;
      ft->dport = dport;
      ft->seq = seq;
      ft->ack = ack;
      ft->ackframe = true;
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
