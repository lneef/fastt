#pragma once

#include <cassert>
#include <cstdint>
#include <message.h>
#include <optional>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_lcore.h>

#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_ring_core.h>
#include <vector>

#include "debug.h"
#include "message.h"
#include "packet_if.h"
#include "protocol.h"
#include "window.h"

#include "retransmission_handler.h"
#include "util.h"

struct statistics {
  uint64_t retransmitted, acked, sent, retransmissions;
  double rtt;
  statistics(uint64_t retransmitted, uint64_t acked, uint64_t sent,
             uint64_t retransmissions, uint64_t rtt_est)
      : retransmitted(retransmitted), acked(acked), sent(sent), retransmissions(retransmissions) {
    rtt = static_cast<double>(rtt_est);
  }

  statistics(): retransmitted(), acked(), sent(), retransmissions(), rtt() {}
};

template <typename D> struct seq_observer {
  void process_seq(uint64_t seq) {
    static_cast<D &>(*this).process_seq_impl(seq);
  }
};

struct ack_scheduler : public seq_observer<ack_scheduler> {
  uint64_t last_acked;
  bool pending_from_retry;
  void process_seq_impl(uint64_t seq) { pending_from_retry = seq < last_acked; }

  bool ack_pending(uint64_t seq) {
    return pending_from_retry || seq > last_acked;
  }

  uint64_t ack_callback(uint64_t seq) {
    pending_from_retry = false;
    return seq - last_acked;
  }

  ack_scheduler() : last_acked(0),  pending_from_retry(false) {}
};

class slotted_transport{
static constexpr uint16_t kOustandingMessages = 128; /* hack for happy path */
  enum class connection_state { ESTABLISHING, ESTABLISHED, DISCONNECTING };
  struct slot_context{
      window<kOustandingMessages / 32> recv_wd;
      ack_scheduler scheduler;
      retransmission_handler rt_handler;
      slot_context(): recv_wd(min_seq), scheduler(), rt_handler(){}
  };
public:
  struct {
    uint64_t sent = 0;
    uint64_t retransmissions = 0;
  } stats;

  slotted_transport(message_allocator *allocator, packet_if *pkt_sink, uint16_t sport,
            const con_config &target)
      : sc(32), target(target), 
        allocator(allocator), pkt_if(pkt_sink), sport(sport) {}

  void probe_timeout(uint16_t tid) {
    sc[tid].rt_handler.probe_retransmit(
        [&](message *msg) { pkt_if->consume_for_retransmission(msg); }, tid);
  }

  bool send_pkt(message *pkt, uint16_t msg_id, bool fini = false) {
    assert(cstate == connection_state::ESTABLISHED);
    auto& sctx = sc[msg_id];
    auto ctor = [&](message *pkt, uint64_t seq) {
      uint64_t ack = 0;
      uint32_t ts = 0;
      auto max_rcvd = sctx.recv_wd.get_last_acked_packet();
      if (sctx.scheduler.ack_pending(max_rcvd)) {
        ack = max_rcvd;
        ts = sctx.recv_wd.get_ts();
        sctx.scheduler.ack_callback(ack);
      }
      protocol::prepare_ft_header(pkt, seq, ack, msg_id, sctx.recv_wd.capacity(),
                                  fini, ts);
    };

    auto inserted = sctx.rt_handler.record_pkt(msg_id, pkt, ctor, budget);
    if (inserted)
      pkt_if->consume_pkt(pkt, sport, target);
    return inserted;
  }

  statistics get_stats() const {
    return {};
  }

  bool acknowledge(uint16_t tid) {
    message *msg;
    bool is_sack = false;
    auto& sctx = sc[tid];
    uint64_t ack = sctx.recv_wd.get_last_acked_packet();
          if (!sctx.scheduler.ack_pending(ack))
        return false;
      msg = allocator->alloc_message(sizeof(protocol::ft_header));
      grant_returned -= sctx.scheduler.ack_callback(ack);
    protocol::prepare_ack_pkt(msg, ack, sctx.recv_wd.capacity(), sctx.recv_wd.get_ts(), is_sack);
    FASTT_LOG_DEBUG("Return %u capacity to peer\n", sctx.recv_wd.capacity());
    pkt_if->consume_pkt(msg, sport, target);
    return true;
  }

  std::optional<uint16_t> process_pkt(message *pkt) {
    auto *hdr = rte_pktmbuf_mtod(pkt, protocol::ft_header *);
    auto ts = *pkt->get_ts() - hdr->ts;
    auto& sctx = sc[hdr->msg_id];
    auto tid = hdr->msg_id;
    std::optional<uint16_t> tid_opt;
    tid_opt.emplace(tid);
    switch (hdr->type) {
    case protocol::pkt_type::FT_MSG: {
      if (hdr->ack)
        budget += sctx.rt_handler.acknowledge(hdr->ack, hdr->wnd, ts);
      sctx.scheduler.process_seq(hdr->seq);
      if (sctx.recv_wd.is_set(hdr->seq)) {
        ++stats.retransmissions;  
        rte_pktmbuf_free(pkt);
        return std::nullopt;
      } else
        sctx.recv_wd.set(hdr->seq, pkt);
      break;
    }
    case protocol::pkt_type::FT_ACK: {
      budget += sctx.rt_handler.acknowledge(hdr->ack, hdr->wnd, ts);
      rte_pktmbuf_free(pkt);
      break;
    }

    case protocol::pkt_type::FT_INIT: {
      if (sctx.recv_wd.is_set(hdr->seq)) {
        rte_pktmbuf_free(pkt);
        return std::nullopt;
      } else
        sctx.recv_wd.set(hdr->seq, pkt);
      setup_after_init();
      cstate = connection_state::ESTABLISHED;
      break;
    }

    case protocol::pkt_type::FT_INIT_ACK: {
      budget += sctx.rt_handler.acknowledge(hdr->ack, hdr->wnd, ts);
      sctx.scheduler.process_seq(hdr->seq);
      if (sctx.recv_wd.is_set(hdr->seq)) {
        rte_pktmbuf_free(pkt);
        return std::nullopt;
      } else {
        sctx.recv_wd.set(hdr->seq, pkt);
      }
      setup_after_init();
      cstate = connection_state::ESTABLISHED;
      break;
    }
    default:
      rte_pktmbuf_free(pkt);
      break;
    }
    return tid_opt;
  }

  void open_connection() {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    bool retval = sc[0].rt_handler.record_pkt(0, msg, [](message *msg, uint64_t seq) {
      protocol::prepare_init_header(msg, seq);
    }, budget);
    assert(retval);
    auto *hdr = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    assert(hdr->type == protocol::FT_INIT);
    FASTT_LOG_DEBUG("Sent init header to peer %u %u\n", target.ip, target.port);
    pkt_if->consume_pkt(msg, sport, target);
  }

  void accept_connection() {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    bool retval = sc[0].rt_handler.record_pkt(
        0, msg, [budget = kOustandingMessages](message *msg, uint64_t seq) {
          protocol::prepare_init_ack_header(msg, seq, min_seq, budget);
        }, budget);
    FASTT_LOG_DEBUG("Sent ack for init");
    assert(retval);
    pkt_if->consume_pkt(msg, sport, target);
  }

  bool active() { return connection_state::ESTABLISHED == cstate; }

  template <typename F> void receive_messages(F &&f, uint16_t tid, uint16_t cnt) {
    grant_returned += sc[tid].recv_wd.advance(f, cnt);
    grant_returned &= (kOustandingMessages - 1);
    /* maybe we lost pkts */
    if (grant_returned >= kOustandingMessages / 4) {
      acknowledge(tid);
    }
  }

  bool has_outstanding_messages(uint16_t tid){
      return sc[tid].recv_wd.least_in_window < sc[tid].recv_wd.max_rx; 
  }

private:
  void setup_after_init() {
    sc[0].recv_wd.advance([](message *msg) { rte_pktmbuf_free(msg); }, 1);
  }

  uint32_t budget = 0;
  std::vector<slot_context> sc;
  con_config target;
  message_allocator *allocator;
  packet_if *pkt_if;
  uint16_t sport;
  uint32_t grant_returned = 0;
  connection_state cstate = connection_state::ESTABLISHING;

};
