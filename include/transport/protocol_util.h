#pragma once
#include "protocol.h"
#include "slab_allocator.h"
#include <rte_mbuf_core.h>
namespace protocol {


template<typename T>
inline T* mtod(rte_mbuf* m, unsigned off = 0){
    return rte_pktmbuf_mtod_offset(m, T*, protocol::defs::kftOffset + off);
}

inline void extract_ports(flow_tuple &ft, rte_mbuf *pkt) {
  auto *hdr = mtod<protocol::ft_header>(pkt);
  ft.sport = hdr->sport;
  ft.dport = hdr->dport;
}

struct msg_frame_desc {
  seq_t seq, ack;
  uint16_t crd;
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
    ft->crd = desc.crd;
    ft->ackframe = desc.ack_frame;
    ft->sack = desc.sack;
    ft->ts = 0;
    ft->type = protocol::pkt_type::FT_MSG;
  }

  inline void prepare_ack_pkt(mbuf *msg, seq_t ack, bool is_sack) {
    auto *ft = msg->data<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->ack = ack;
    ft->sack = is_sack;
    ft->crd = 0;
    ft->ts = 0;
    ft->type = protocol::pkt_type::FT_ACK;
  }

  inline void prepare_init_header(mbuf *msg, seq_t seq, uint16_t budget) {
    auto *ft = msg->data<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->seq = seq;
    ft->ackframe = 0;
    ft->sack = 0;
    ft->crd = budget;
    ft->ts = 0;
    ft->type = protocol::pkt_type::FT_SYN;
  }

  inline void prepare_ctrl_pkt(mbuf *msg, seq_t seq, seq_t ack, uint16_t wnd,
                               bool is_ack_frame) {
    auto *ft = msg->data<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->seq = seq;
    ft->ack = ack;
    ft->crd = wnd;
    ft->ackframe = is_ack_frame;
    ft->ts = 0;
    ft->type = protocol::pkt_type::FT_CRD_UPDATE;
  }

  inline void prepare_init_ack_header(mbuf *msg, seq_t seq, seq_t ack,
                                      uint16_t wnd, bool is_ack_frame) {
    auto *ft = msg->data<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->ack = ack;
    ft->crd = wnd;
    ft->seq = seq;
    ft->ackframe = is_ack_frame;
    ft->sack = 0;
    ft->ts = 0;
    ft->type = protocol::pkt_type::FT_SYN_ACK;
  }

  inline void prepare_done_header(mbuf *msg, seq_t seq, seq_t ack,
                                  bool is_ack_frame) {
    auto *ft = msg->data<protocol::ft_header>();
    ft->sport = sport;
    ft->dport = dport;
    ft->seq = seq;
    ft->ack = ack;
    ft->ackframe = is_ack_frame;
    ft->ts = 0;
    ft->type = protocol::pkt_type::FT_DONE;
  }
};
} // namespace protocol
