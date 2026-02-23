#pragma once

#include <cassert>
#include <cerrno>
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
#include <sys/types.h>

#include "debug.h"
#include "message.h"
#include "packet_if.h"
#include "protocol.h"

#include "transport/seq.h"
#include "transport_input.h"
#include "transport_output.h"
#include "util.h"

struct transport_statistics {
  uint64_t retransmitted;
  seq_t acked;
  uint64_t sent, retransmissions;
  double rtt;
  transport_statistics(uint64_t retransmitted, seq_t acked, uint64_t sent,
                       uint64_t retransmissions, uint64_t rtt_est)
      : retransmitted(retransmitted), acked(acked), sent(sent),
        retransmissions(retransmissions) {
    rtt = static_cast<double>(rtt_est);
  }

  transport_statistics()
      : retransmitted(), acked(), sent(), retransmissions(), rtt() {}
};

template <typename D> struct seq_observer {
  void process_seq(seq_t seq) { static_cast<D &>(*this).process_seq_impl(seq); }
};

struct ack_scheduler : public seq_observer<ack_scheduler> {
  seq_t last_acked;
  uint64_t last_sack;
  bool pending_from_retry;
  void process_seq_impl(seq_t seq) { pending_from_retry = seq < last_acked; }

  bool ack_pending(seq_t seq) { return pending_from_retry || seq > last_acked; }

  bool sack_pending(uint64_t rcvd_pkts) {
    return pending_from_retry || last_sack != rcvd_pkts;
  }

  void ack_callback(seq_t seq) {
    last_acked = seq;
    pending_from_retry = false;
  }

  void sack_callback(seq_t seq, uint64_t rcvd_pkts) {
    last_acked = seq;
    last_sack = rcvd_pkts;
    pending_from_retry = false;
  }

  ack_scheduler() : last_acked(), last_sack(), pending_from_retry(false) {}
};

enum class connection_state { ESTABLISHING, ESTABLISHED, DISCONNECTING, DISCONNECTED };

class connection;
class transport {
  friend class connection;

public:
  struct {
    uint64_t sent = 0;
    uint64_t retransmissions = 0;
  } stats;

  transport(message_allocator *allocator, packet_if *pkt_sink,
            transport_config cfg, uint16_t sport, uint16_t dport)
      : trx(allocator), builder(sport, dport), cfg(cfg), ttx(), scheduler(),
        allocator(allocator), pkt_if(pkt_sink) {}

  void probe_timeout() {
    ttx.probe_retransmit(
        [&](message *msg) { pkt_if->consume_for_retransmission(msg); });
  }

  void check_timeout(uint64_t now) {
    if (cstate == connection_state::DISCONNECTED)
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
    ttx.record_ctrl_pkt(msg, [&](message *msg, seq_t seq) {
      builder.prepare_ctrl_pkt(msg, seq, wnd);
    });
    pkt_if->consume_pkt(msg, cfg);
  }

  bool acknowledge(uint64_t now = rte_get_timer_cycles()) {
    message *msg;
    bool is_sack = trx.has_holes();
    seq_t ack = trx.get_last_rcvd_in_seq();
    auto total_rcvd_pkts = trx.get_total_rcvd_pkts();
    if (is_sack) {
      if (!scheduler.sack_pending(total_rcvd_pkts))
        return false;
      msg = allocator->alloc_message(sizeof(protocol::ft_header) +
                                     sizeof(protocol::ft_sack_payload));
      auto *sack_payload = rte_pktmbuf_mtod_offset(
          msg, protocol::ft_sack_payload *, sizeof(protocol::ft_header));
      trx.copy_bitset(sack_payload);
      scheduler.sack_callback(ack, total_rcvd_pkts);
      FASTT_LOG_DEBUG("Sending SACK of size %u with contiguos ack until %u\n",
                      sack_payload->bit_map_len, ack.v);
    } else {
      if (!scheduler.ack_pending(ack))
        return false;
      msg = allocator->alloc_message(sizeof(protocol::ft_header));
      scheduler.ack_callback(ack);
      FASTT_LOG_DEBUG("Sending ACK ack=%u\n", ack.v);
    }
    if(cstate == connection_state::DISCONNECTING)
        cstate = connection_state::DISCONNECTED;

    builder.prepare_ack_pkt(msg, ack, now / get_ticks_us() - trx.get_ts(),
                            is_sack);
    pkt_if->consume_pkt(msg, cfg);
    return true;
  }

  bool process_pkt(message *msg) {
    auto *hdr = msg->data<const protocol::ft_header>();
    auto ts = *msg->get_ts() - hdr->ts;
    switch (hdr->type) {
    case protocol::pkt_type::FT_MSG: {
      FASTT_LOG_DEBUG("Got new msg seq=%u ack=%u ackframe=%u wnd=%u\n", hdr->seq.v, hdr->ack.v, hdr->ackframe, hdr->wnd);
      scheduler.process_seq(hdr->seq);
      if (trx.is_retransmission_or_exceeds_capacity(hdr->seq,
                                                    stats.retransmissions)) {
        msg->free();
        return false;
      }
      if (hdr->ackframe)
        ttx.acknowledge(hdr->ack, ts, hdr->sack);
      if (hdr->wnd)
        ttx.update_budget(hdr->wnd);
      trx.insert(hdr->seq, msg);
      break;
    }
    case protocol::pkt_type::FT_ACK: {
      FASTT_LOG_DEBUG("Got ACK ack=%u sack=%u\n", hdr->ack.v, hdr->sack);
      ttx.acknowledge(hdr->ack, ts, hdr->sack);
      if (hdr->sack) {
        auto *sack_payload =
            msg->data<protocol::ft_sack_payload>(sizeof(protocol::ft_header));
        ttx.acknowledge_sack(sack_payload, ts, [&](message *msg) {
          pkt_if->consume_for_retransmission(msg);
        });
      }
      if (cstate == connection_state::DISCONNECTING && ttx.all_acked())
        cstate = connection_state::DISCONNECTED;
      assert(hdr->wnd == 0);
      msg->free();
      break;
    }
    case protocol::pkt_type::FT_RDY_TO_RCV: {
      FASTT_LOG_DEBUG("Got RDY_TO_RCV seq=%u wnd=%u\n", hdr->seq.v, hdr->wnd);
      scheduler.process_seq(hdr->seq);
      if (trx.is_retransmission_or_exceeds_capacity(hdr->seq,
                                                    stats.retransmissions)) {
        msg->free();
        return false;
      }
      ttx.update_budget(hdr->wnd);
      trx.insert(hdr->seq, msg);
      cstate = connection_state::ESTABLISHED;
      break;
    }
    case protocol::pkt_type::FT_CLR_TO_SD: {
      FASTT_LOG_DEBUG("Got CLR_TO_SD seq=%u ack=%u wnd=%u\n", hdr->seq.v, hdr->ack.v, hdr->wnd);
      scheduler.process_seq(hdr->seq);
      if (trx.is_retransmission_or_exceeds_capacity(hdr->seq,
                                                    stats.retransmissions)) {
        msg->free();
        return false;
      }
      ttx.acknowledge(hdr->ack, ts, hdr->sack);
      assert(hdr->wnd > 0);
      ttx.update_budget(hdr->wnd);
      trx.insert(hdr->seq, msg);
      cstate = connection_state::ESTABLISHED;
      break;
    }
    case protocol::pkt_type::FT_WND_RET: {
      FASTT_LOG_DEBUG("Got WND_RET seq=%u wnd=%u\n", hdr->seq.v, hdr->wnd);
      scheduler.process_seq(hdr->seq);
      if (trx.is_retransmission_or_exceeds_capacity(hdr->seq,
                                                    stats.retransmissions)) {
        msg->free();
        return false;
      }
      ttx.update_budget(hdr->wnd);
      trx.insert(hdr->seq, msg);
      break;
    }
    case protocol::pkt_type::FT_DONE: {
      FASTT_LOG_DEBUG("Got DONE seq=%u ack=%u\n", hdr->seq.v, hdr->ack.v);
      scheduler.process_seq(hdr->seq);
      if (trx.is_retransmission_or_exceeds_capacity(hdr->seq,
                                                    stats.retransmissions)) {
        msg->free();
        return false;
      }
      ttx.acknowledge(hdr->ack, hdr->ts, hdr->sack);
      // for now we assume all rpc/exchange has completed
      // ack for FT_DONE will be sent from the event loop
      assert(ttx.all_acked());
      assert(trx.empty());
      cstate = connection_state::DISCONNECTING;
      break;
    }
    default:
      msg->free();
      break;
    }
    return true;
  }

  void close_connection() {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    ttx.record_ctrl_pkt(msg, [&](message *msg, seq_t seq) {
      scheduler.ack_callback(trx.get_last_rcvd_in_seq());
      builder.prepare_done_header(msg, seq, trx.get_last_rcvd_in_seq());
    });
    cstate = connection_state::DISCONNECTING;
    pkt_if->consume_pkt(msg, cfg);
  }

  void open_connection() {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    assert(msg);
    ttx.record_ctrl_pkt(
        msg, [&, budget = trx.prepare_wnd_return()](message *msg, seq_t seq) {
          builder.prepare_init_header(msg, seq, budget);
        });
    auto *hdr = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    assert(hdr->type == protocol::FT_RDY_TO_RCV);
    FASTT_LOG_DEBUG("Sent RDY_TO_RCV seq=%u wnd=%u flow=%s\n", hdr->seq.v, hdr->wnd, get_flow_tuple().print().c_str());
    pkt_if->consume_pkt(msg, cfg);
  }

  void accept_connection() {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    assert(msg);
    ttx.record_ctrl_pkt(
        msg, [&, budget = trx.prepare_wnd_return()](message *msg, seq_t seq) {
          builder.prepare_init_ack_header(msg, seq, seq, budget);
        });
    auto *hdr = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    FASTT_LOG_DEBUG("Sent CLR_TO_SD seq=%u ack=%u wnd=%u flow=%s\n", hdr->seq.v, hdr->ack.v, hdr->wnd, get_flow_tuple().print().c_str());
    pkt_if->consume_pkt(msg, cfg);
  }

  bool up() { return connection_state::ESTABLISHED == cstate; }

  bool disconnected() { return connection_state::DISCONNECTED == cstate; }

  template <typename F> void receive_messages(F &&f) { trx.advance(f); }

  bool can_recv() {
    return trx.has_buffered_messages_frags() ||
           cstate != connection_state::ESTABLISHED;
  }

  bool can_send() {
    return ttx.get_current_wnd() > 0 || cstate != connection_state::ESTABLISHED;
  }

  ssize_t send_single(struct iovec *iov, bool som, bool eom) {
    static constexpr uint16_t kMaxPayload = 1500 - protocol::defs::kHeaderMTUlen;
    if(iov->iov_len > kMaxPayload)
        return -ENOMEM;
    if(!ttx.get_current_wnd())
        return -EAGAIN;
    if (connection_state::ESTABLISHED != cstate)
      return 0;
    auto *mbuf = allocator->alloc_message(iov->iov_len);
    rte_memcpy(mbuf->data<uint8_t>(), iov->iov_base, iov->iov_len);
    auto ctor = [&](message *pkt, seq_t seq) {
      uint16_t wnd = trx.prepare_wnd_return();
      seq_t ack = trx.get_last_rcvd_in_seq();
      bool is_ack_frame = true;
      uint32_t ts = rte_get_timer_cycles() / get_ticks_us() - trx.get_ts();
      scheduler.ack_callback(ack);

      builder.prepare_ft_header(pkt, seq, ack, wnd, som, eom, ts,
                                is_ack_frame, false);
    };
    auto inserted = ttx.record_pkt(mbuf, ctor);
    if (inserted)
      pkt_if->consume_pkt(mbuf, cfg);
    return iov->iov_len;
  }

  ssize_t send(msg_hdr& hdr){
      ssize_t sent = 0;
      for(uint16_t i = 0; i < hdr.iov_len; ++i){
          auto retval = send_single(&hdr.iov[i], i == 0, i == hdr.iov_len - 1);
          if(retval <= 0){
              hdr.flags = retval;
              FASTT_LOG_DEBUG("send failed iov=%u retval=%zd\n", i, retval);
              return retval;
          }
          sent += retval;
      }
      FASTT_LOG_DEBUG("send iov_len=%u total=%zd\n", hdr.iov_len, sent);
      return sent;
  }

  ssize_t recv(msg_hdr &hdr) {
    if (connection_state::ESTABLISHED != cstate)
      return 0;
    auto ret = trx.read(hdr);
    FASTT_LOG_DEBUG("recv ret=%zd\n", ret);
    return ret;
  }

  transport_statistics get_stats() const {
    auto &rt_stats = ttx.get_stats();
    return {rt_stats.retransmitted, rt_stats.acked, stats.sent,
            stats.retransmissions, rt_stats.rtt};
  }

  flow_tuple get_flow_tuple() const{
      flow_tuple ft;
      ft.dport = builder.sport;
      ft.sport = builder.dport;
      ft.sip = cfg.ip;
      return ft;
  }

private:
  transport_output trx;
  protocol::builder builder;
  transport_config cfg;
  transport_input ttx;
  ack_scheduler scheduler;
  message_allocator *allocator;
  packet_if *pkt_if;
  uint64_t rto = get_ticks_ms() * 10;
  connection_state cstate = connection_state::ESTABLISHING;
};
