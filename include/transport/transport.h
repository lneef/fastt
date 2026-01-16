#pragma once

#include <cassert>
#include <cstdint>
#include <message.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_lcore.h>

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_ring_core.h>

#include "log.h"
#include "message.h"
#include "packet_if.h"
#include "protocol.h"

#include "util.h"

struct receiver_entry {
  uint64_t seq;
  receiver_entry() : seq(0) {}
};

template <typename D> struct ack_observer {
  void process_ack(uint64_t ack_seq, uint64_t now) {
    static_cast<D *>(this)->process_ack_impl(ack_seq, now);
  }
};

struct statistics {
  uint64_t retransmitted, acked, sent, ecn;
  double rtt;
  statistics(uint64_t retransmitted, uint64_t acked, uint64_t sent,
             uint64_t ecn, uint64_t rtt_est)
      : retransmitted(retransmitted), acked(acked), sent(sent), ecn(ecn) {
    rtt = static_cast<double>(rtt_est) / (rte_get_timer_hz() / 1e6);
  }
};

struct transport {
  message_allocator *allocator;
  packet_if *pkt_if;
  uint16_t sport;
  con_config target;
  struct {
    uint64_t sent = 0;
    uint64_t with_ecn = 0;
  } stats;

  transport(message_allocator *allocator, packet_if *pkt_sink, uint16_t sport, const con_config& target)
      : allocator(allocator), pkt_if(pkt_sink), sport(sport), target(target) {}

  void send_ack(uint32_t msg_id, uint32_t seq, uint16_t wnd){
      auto *ack = protocol::prepare_ack_header(seq, allocator, wnd, msg_id);
      pkt_if->consume_pkt(ack, sport, target);
  }

  message* prepare_init(uint64_t seq, uint16_t wnd){
      auto *init = protocol::prepare_init_header(seq, allocator, wnd);
      return init;
  }

  void retransmit(message* msg){
      pkt_if->consume_for_retransmission(msg);
  }

  void send_pkt(message* msg){
      pkt_if->consume_pkt(msg, sport, target);
  }
};
