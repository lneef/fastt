#pragma once

#include <array>
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
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_ring_core.h>

#include "debug.h"
#include "message.h"
#include "packet_if.h"
#include "protocol.h"
#include "transport/filter.h"
#include "window.h"

#include "retransmission_handler.h"
#include "util.h"

struct transport_statistics {
  uint64_t retransmitted, acked, sent, retransmissions;
  double rtt;
  transport_statistics(uint64_t retransmitted, uint64_t acked, uint64_t sent,
             uint64_t retransmissions, uint64_t rtt_est)
      : retransmitted(retransmitted), acked(acked), sent(sent), retransmissions(retransmissions) {
    rtt = static_cast<double>(rtt_est);
  }

  transport_statistics(): retransmitted(), acked(), sent(), retransmissions(), rtt() {}
};

template <typename D> struct seq_observer {
  void process_seq(uint64_t seq) {
    static_cast<D &>(*this).process_seq_impl(seq);
  }
};

struct ack_scheduler : public seq_observer<ack_scheduler> {
  uint64_t last_acked;
  uint64_t last_sack;
  bool pending_from_retry;
  void process_seq_impl(uint64_t seq) { pending_from_retry = seq < last_acked; }

  bool ack_pending(uint64_t seq) {
    return pending_from_retry || seq > last_acked;
  }

  bool sack_pending(uint64_t seq) {
    return pending_from_retry || seq > last_sack;
  }

  void ack_callback(uint64_t seq) {
    last_acked = seq;
    pending_from_retry = false;
  }

  void sack_callback(uint64_t seq) {
    last_acked = seq;
    pending_from_retry = false;
  }

  ack_scheduler() : last_acked(0), last_sack(1), pending_from_retry(false) {}
};

class connection;
class transport {
  static constexpr uint16_t kOustandingMessages = 4;
  static constexpr uint16_t kNumStreams = 4;
  friend class connection;
  enum class connection_state { ESTABLISHING, ESTABLISHED, DISCONNECTING };
  struct stream{
      retransmission_handler rt_handler;
      ack_scheduler scheduler;
      window <kOustandingMessages> recv_wd;
      stream(): rt_handler(kOustandingMessages, kOustandingMessages), recv_wd(min_seq) {}
  };
public:
  struct {
    uint64_t sent = 0;
    uint64_t retransmissions = 0;
  } stats;

  transport(message_allocator *allocator, packet_if *pkt_sink, uint16_t sport,
            const con_config &target)
      : streams(kNumStreams), target(target), 
        allocator(allocator), pkt_if(pkt_sink), sport(sport) {}

  void probe_timeout(uint16_t tid) {
    streams[tid].rt_handler.probe_retransmit(
        [&](message *msg) { pkt_if->consume_for_retransmission(msg); });
  }

  bool send_pkt(message *pkt, uint16_t tid, bool fini = false) {
    assert(cstate == connection_state::ESTABLISHED);
    auto& [rt_handler, scheduler, recv_wd] = streams[tid];
    auto ctor = [&](message *pkt, uint64_t seq) {
      uint64_t ack = 0;
      uint32_t ts = 0;
      auto least_in_window = recv_wd.get_last_acked_packet();
      if (scheduler.ack_pending(least_in_window)) {
        ack = least_in_window;
        ts = recv_wd.get_ts();
        scheduler.ack_callback(ack);
      }
      protocol::prepare_ft_header(pkt, seq, ack, tid, recv_wd.capacity(),
                                  fini, ts);
    };

    auto inserted = rt_handler.record_pkt(tid, pkt, ctor);
    if (inserted)
      pkt_if->consume_pkt(pkt, sport, target);
    return inserted;
  }

  transport_statistics get_stats() const {
    transport_statistics stats{};  
    for(auto &st : streams){
        
    auto &rt_stats = st.rt_handler.get_stats();
    stats.retransmitted += rt_stats.retransmitted;
    stats.acked += rt_stats.acked;
    stats.rtt = filter::exp_filter<uint64_t>(stats.rtt, rt_stats.rtt);
    }
    return stats;
  }

  bool acknowledge(uint16_t tid) {
    message *msg;
    auto& [_, scheduler, recv_wd] = streams[tid];
    bool is_sack = recv_wd.has_holes();
    uint64_t ack = recv_wd.get_last_acked_packet();
    if (is_sack) {
      if (!scheduler.sack_pending(ack))
        return false;
      msg = allocator->alloc_message(sizeof(protocol::ft_header) +
                                     sizeof(protocol::ft_sack_payload));
      auto *sack_payload = rte_pktmbuf_mtod_offset(
          msg, protocol::ft_sack_payload *, sizeof(protocol::ft_header));
      recv_wd.copy_bitset(sack_payload);
      scheduler.sack_callback(ack);
      FASTT_LOG_DEBUG("Sending SACK of size %u with contiguos ack until %lu\n", sack_payload->bit_map_len, ack);
    } else {
      if (!scheduler.ack_pending(ack))
        return false;
      msg = allocator->alloc_message(sizeof(protocol::ft_header));
      scheduler.ack_callback(ack);
    }
    protocol::prepare_ack_pkt(msg, ack, recv_wd.capacity(), recv_wd.get_ts(), is_sack);
    FASTT_LOG_DEBUG("Return %u capacity to peer\n", recv_wd.capacity());
    pkt_if->consume_pkt(msg, sport, target);
    return true;
  }

  bool process_pkt(message *pkt) {
    auto *hdr = rte_pktmbuf_mtod(pkt, protocol::ft_header *);
    auto ts = *pkt->get_ts() - hdr->ts;
    auto& [rt_handler, scheduler, recv_wd] = streams[hdr->msg_id];
    switch (hdr->type) {
    case protocol::pkt_type::FT_MSG: {
      if (hdr->ack)
        rt_handler.acknowledge(hdr->ack, hdr->wnd, ts);
      scheduler.process_seq(hdr->seq);
      if (recv_wd.is_set(hdr->seq)) {
        ++stats.retransmissions;  
        rte_pktmbuf_free(pkt);
        return false;
      } else
        recv_wd.set(hdr->seq, pkt);
      break;
    }
    case protocol::pkt_type::FT_ACK: {
      rt_handler.acknowledge(hdr->ack, hdr->wnd, ts);
      rte_pktmbuf_free(pkt);
      break;
    }
    case protocol::pkt_type::FT_INIT: {
      if (recv_wd.is_set(hdr->seq)) {
        rte_pktmbuf_free(pkt);
        return false;
      } else
        recv_wd.set(hdr->seq, pkt);
      setup_after_init();
      cstate = connection_state::ESTABLISHED;
      break;
    }
    case protocol::pkt_type::FT_INIT_ACK: {
      rt_handler.acknowledge(hdr->ack, hdr->wnd, ts);
      scheduler.process_seq(hdr->seq);
      if (recv_wd.is_set(hdr->seq)) {
        rte_pktmbuf_free(pkt);
        return false;
      } else {
        recv_wd.set(hdr->seq, pkt);
      }
      setup_after_init();
      cstate = connection_state::ESTABLISHED;
      break;
    }
    default:
      rte_pktmbuf_free(pkt);
      break;
    }
    return true;
  }

  void open_connection() {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    bool retval = streams[0].rt_handler.record_pkt(0, msg, [](message *msg, uint64_t seq) {
      protocol::prepare_init_header(msg, seq);
    });
    assert(retval);
    auto *hdr = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    assert(hdr->type == protocol::FT_INIT);
    FASTT_LOG_DEBUG("Sent init header to peer %u %u\n", target.ip, target.port);
    pkt_if->consume_pkt(msg, sport, target);
  }

  void accept_connection() {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    auto &[rt_handler, _, recv_wd] = streams[0];
    bool retval = rt_handler.record_pkt(
        0, msg, [budget = recv_wd.capacity()](message *msg, uint64_t seq) {
          protocol::prepare_init_ack_header(msg, seq, min_seq, kOustandingMessages);
        });
    FASTT_LOG_DEBUG("Sent ack for init");
    assert(retval);
    pkt_if->consume_pkt(msg, sport, target);
  }

  bool active() { return connection_state::ESTABLISHED == cstate; }

  template <typename F> void receive_messages(F &&f) {
    for(auto& st: streams)  
        grant_returned += st.recv_wd.advance(f);
  }

private:
  void setup_after_init() {
    streams[0].recv_wd.advance([](message *msg) { rte_pktmbuf_free(msg); });
  }

  std::vector<stream> streams;
  con_config target;
  message_allocator *allocator;
  packet_if *pkt_if;
  uint16_t sport;
  uint32_t grant_returned = 0;
  connection_state cstate = connection_state::ESTABLISHING;
};
