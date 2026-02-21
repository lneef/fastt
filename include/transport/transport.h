#pragma once

#include <cassert>
#include <cstdint>
#include <generic/rte_cycles.h>
#include <message.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_lcore.h>

#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memcpy.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_ring_core.h>

#include "debug.h"
#include "message.h"
#include "packet_if.h"
#include "protocol.h"

#include "transport_input.h"
#include "transport_output.h"
#include "util.h"

struct transport_statistics {
  uint64_t retransmitted, acked, sent, retransmissions;
  double rtt;
  transport_statistics(uint64_t retransmitted, uint64_t acked, uint64_t sent,
                       uint64_t retransmissions, uint64_t rtt_est)
      : retransmitted(retransmitted), acked(acked), sent(sent),
        retransmissions(retransmissions) {
    rtt = static_cast<double>(rtt_est);
  }

  transport_statistics()
      : retransmitted(), acked(), sent(), retransmissions(), rtt() {}
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
    last_sack = seq;
    pending_from_retry = false;
  }

  ack_scheduler() : last_acked(0), last_sack(1), pending_from_retry(false) {}
};

class connection;
class transport {
  friend class connection;
  enum class connection_state { ESTABLISHING, ESTABLISHED, DISCONNECTING };

public:
  struct {
    uint64_t sent = 0;
    uint64_t retransmissions = 0;
  } stats;

  transport(message_allocator *allocator, packet_if *pkt_sink, uint16_t sport,
            const con_config &target)
      : trx(min_seq, allocator), target(target), ttx(), scheduler(),
        allocator(allocator), pkt_if(pkt_sink), sport(sport) {}

  void probe_timeout() {
    ttx.probe_retransmit(
        [&](message *msg) { pkt_if->consume_for_retransmission(msg); });
  }

  void check_timeout(uint64_t now) {
    if (cstate != connection_state::ESTABLISHED)
      return;
    if (ttx.all_acked())
      return;
    if (ttx.check_timeout(now)) {
      probe_timeout();
      ttx.rearm(now);
    }
  }

  void check_ctrl() {
    if (trx.check_wnd_return())
      send_ctrl(trx.prepare_wnd_return());
  }

  void send_ctrl(uint16_t wnd) {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    if (!msg)
      return;
    ttx.record_ctrl_pkt(msg, [&](message *msg, uint64_t seq) {
      protocol::prepare_ctrl_pkt(msg, seq, wnd);
    });
    pkt_if->consume_pkt(msg, sport, target);
  }

  size_t send(void *buf, size_t size, bool start, bool end) {
    static constexpr uint16_t kMaxPayload = 2048;
    assert(size < kMaxPayload);
    assert(capacity());
    auto *mbuf = allocator->alloc_message(size);
    rte_memcpy(mbuf->data<uint8_t>(), buf, size);
    auto ctor = [&](message *pkt, uint64_t seq) {
      uint64_t ack = 0;
      uint32_t ts = 0;
      uint16_t wnd = trx.prepare_wnd_return();
      if (scheduler.ack_pending(seq)) {
        ack = trx.get_last_rcvd_in_seq();
        ts = rte_get_timer_cycles() / get_ticks_us() - trx.get_ts();
        scheduler.ack_callback(ack);
      }

      protocol::prepare_ft_header(pkt, seq, ack, wnd, start, end, ts, false);
    };
    auto inserted = ttx.record_pkt(mbuf, ctor);
    if (inserted)
      pkt_if->consume_pkt(mbuf, sport, target);
    return size;
  }

  bool acknowledge(uint64_t now = rte_get_timer_cycles()) {
    message *msg;
    bool is_sack = trx.has_holes();
    uint64_t ack = trx.get_last_rcvd_in_seq();
    if (is_sack) {
      if (!scheduler.sack_pending(trx.max_rx_in_window))
        return false;
      msg = allocator->alloc_message(sizeof(protocol::ft_header) +
                                     sizeof(protocol::ft_sack_payload));
      auto *sack_payload = rte_pktmbuf_mtod_offset(
          msg, protocol::ft_sack_payload *, sizeof(protocol::ft_header));
      trx.copy_bitset(sack_payload);
      scheduler.sack_callback(trx.max_rx_in_window);
      FASTT_LOG_DEBUG("Sending SACK of size %u with contiguos ack until %lu\n",
                      sack_payload->bit_map_len, ack);
    } else {
      if (!scheduler.ack_pending(ack))
        return false;
      msg = allocator->alloc_message(sizeof(protocol::ft_header));
      scheduler.ack_callback(ack);
    }
    FASTT_LOG_DEBUG("Return %u capacity to peer\n", trx.get_available_wnd());
    protocol::prepare_ack_pkt(msg, ack, now / get_ticks_us() - trx.get_ts(),
                              is_sack);
    pkt_if->consume_pkt(msg, sport, target);
    return true;
  }

  bool process_pkt(message *msg) {
    auto *hdr = msg->data<protocol::ft_header>();
    auto ts = *msg->get_ts() - hdr->ts;
    switch (hdr->type) {
    case protocol::pkt_type::FT_MSG: {
      if (hdr->ack)
        ttx.acknowledge(hdr->ack, ts, hdr->sack);
      if (hdr->wnd)
        ttx.update_budget(hdr->wnd);

      scheduler.process_seq(hdr->seq);
      if (trx.is_set(hdr->seq)) {
        ++stats.retransmissions;
        msg->free();
        return false;
      } else
        trx.set(hdr->seq, msg);
      break;
    }
    case protocol::pkt_type::FT_ACK: {
      ttx.acknowledge(hdr->ack, ts, hdr->sack);
      if (hdr->sack) {
        auto *sack_payload =
            msg->data<protocol::ft_sack_payload>(sizeof(protocol::ft_header));
        ttx.acknowledge_sack(sack_payload, ts, [&](message *msg) {
          pkt_if->consume_for_retransmission(msg);
        });
      }
      if (hdr->ack == min_seq)
        cstate = connection_state::ESTABLISHED;
      assert(hdr->wnd == 0);
      msg->free();
      break;
    }
    case protocol::pkt_type::FT_INIT: {
      if (trx.is_set(hdr->seq)) {
        msg->free();
        return false;
      } else
        trx.set(hdr->seq, msg);
      if (hdr->wnd)
        ttx.update_budget(hdr->wnd);
      setup_after_init();
      break;
    }
    case protocol::pkt_type::FT_INIT_ACK: {
      ttx.acknowledge(hdr->ack, ts, hdr->sack);
      scheduler.process_seq(hdr->seq);
      if (trx.is_set(hdr->seq)) {
        msg->free();
        return false;
      } else {
        trx.set(hdr->seq, msg);
      }
      assert(hdr->wnd > 0);
      ttx.update_budget(hdr->wnd);
      setup_after_init();
      cstate = connection_state::ESTABLISHED;
      break;
    }
    case protocol::pkt_type::FT_CRTL: {
      if (trx.is_set(hdr->seq)) {
        msg->free();
        return false;
      }
      trx.set(hdr->seq, msg);
      assert(hdr->wnd > 0);
      ttx.update_budget(hdr->wnd);
      msg->free();
      break;
    };
    default:
      msg->free();
      break;
    }
    return true;
  }

  void open_connection() {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    assert(msg);
    ttx.record_ctrl_pkt(
        msg, [budget = trx.prepare_wnd_return()](message *msg, uint64_t seq) {
          protocol::prepare_init_header(msg, seq, budget);
        });
    auto *hdr = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    assert(hdr->type == protocol::FT_INIT);
    FASTT_LOG_DEBUG("Sent init header to peer %u %u\n", target.ip, target.port);
    pkt_if->consume_pkt(msg, sport, target);
  }

  void accept_connection() {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    assert(msg);
    ttx.record_ctrl_pkt(
        msg, [budget = trx.prepare_wnd_return()](message *msg, uint64_t seq) {
          protocol::prepare_init_ack_header(msg, seq, min_seq, budget);
        });
    FASTT_LOG_DEBUG("Sent ack for init");
    pkt_if->consume_pkt(msg, sport, target);
  }

  bool active() { return connection_state::ESTABLISHED == cstate; }

  template <typename F> void receive_messages(F &&f) { trx.advance(f); }

  unsigned capacity() { return ttx.get_current_wnd(); }

  bool can_recv() { return trx.has_buffered_messages_frags(); }

  ssize_t recv(msg_hdr &hdr) { return trx.read(hdr); }

  transport_statistics get_stats() const {
    auto &rt_stats = ttx.get_stats();
    return {rt_stats.retransmitted, rt_stats.acked, stats.sent,
            stats.retransmissions, rt_stats.rtt};
  }

private:
  void setup_after_init() {
    trx.advance([](message *msg) {
      msg->free();
      return nullptr;
    });
  }

  transport_output trx;
  con_config target;
  transport_input ttx;
  ack_scheduler scheduler;
  message_allocator *allocator;
  packet_if *pkt_if;
  uint16_t sport;
  uint64_t rto = get_ticks_ms() * 10;
  connection_state cstate = connection_state::ESTABLISHING;
};
