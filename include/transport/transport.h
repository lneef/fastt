#pragma once

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <generic/rte_cycles.h>
#include <msg_fragment.h>
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
#include "msg_fragment.h"
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
  bool pending_from_retry;
  void process_seq_impl(seq_t seq) { pending_from_retry |= seq < last_acked; }

  bool ack_pending(seq_t seq) {
    return seq > last_acked || pending_from_retry;  
  }

  void ack_callback(seq_t seq) {
    last_acked = seq;
    pending_from_retry = false;
  }

  void sack_callback(seq_t seq) {
    last_acked = seq;
    pending_from_retry = false;
  }

  ack_scheduler() : last_acked(~0u), pending_from_retry(false) {}
};

enum class connection_state {
  ESTABLISHING,
  ESTABLISHED,
  DISCONNECTING,
  DISCONNECTED
};

class connection;
template <typename P = packet_if> class transport {
  friend class connection;

public:
  static constexpr uint16_t kMaxPayload = 1500 - protocol::defs::kHeaderMTUlen;
  struct {
    uint64_t sent = 0;
    uint64_t retransmissions = 0;
  } stats;

  transport(msg_fragment_allocator *allocator, P *pkt_sink,
            transport_config cfg, uint16_t sport, uint16_t dport)
      : trx(allocator), builder(sport, dport), cfg(cfg), ttx(), scheduler(),
        allocator(allocator), pkt_if(pkt_sink) {}

  void perform_recovery(){
      ttx.advance_recovery([&](msg_fragment* mf){
              pkt_if->consume_for_retransmission(mf);
              });
  }

  void check_timeout(uint64_t now) {
    if (cstate == connection_state::DISCONNECTED)
      return;
    if (ttx.all_acked())
      return;
    if (ttx.check_timeout(now)) {
      ttx.rto_retransmit(now);
      ttx.rearm(now);
    }
  }

  void check_ctrl() {
    if (trx.check_wnd_return())
      send_ctrl(trx.prepare_wnd_return());
  }

  void send_ctrl(uint16_t wnd) {
    auto *msg = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
    if (!msg)
      return;
    auto now = rte_get_timer_cycles();
    ttx.record_ctrl_pkt(msg, [&](msg_fragment *msg, seq_t seq) {
        auto ack = trx.get_last_rcvd_in_seq();
      bool ackframe = scheduler.ack_pending(ack);     
      if(ackframe)
        scheduler.ack_callback(ack);
      builder.prepare_ctrl_pkt(msg, seq, ack, wnd, ackframe);
    }, now);
    pkt_if->consume_pkt(msg, cfg);
  }

  bool acknowledge() {
    msg_fragment *msg;
    bool is_sack = trx.has_holes();
    seq_t ack = trx.get_last_rcvd_in_seq();
    if (is_sack) {
      msg = allocator->alloc_msg_fragment(sizeof(protocol::ft_header) +
                                          sizeof(protocol::ft_sack_payload));
      if(!msg)
          return false;
      auto *sack_payload = rte_pktmbuf_mtod_offset(
          msg, protocol::ft_sack_payload *, sizeof(protocol::ft_header));
      trx.copy_bitset(sack_payload);
      scheduler.sack_callback(ack);
      FASTT_LOG_DEBUG("Sending SACK of size %u with contiguos ack until %u\n",
                      sack_payload->bit_map_len, ack.v);
    } else {
      if (!scheduler.ack_pending(ack))
        return false;
      msg = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
      if(!msg)
          return false;
      scheduler.ack_callback(ack);
      FASTT_LOG_DEBUG("Sending ACK ack=%u\n", ack.v);
    }
    if (cstate == connection_state::DISCONNECTING)
      cstate = connection_state::DISCONNECTED;

    builder.prepare_ack_pkt(msg, ack,
                            is_sack);
    pkt_if->consume_pkt(msg, cfg);
    return true;
  }

  bool process_pkt(msg_fragment *msg) {
    auto *hdr = msg->data<const protocol::ft_header>();
    auto ts = *msg->get_ts();
    switch (hdr->type) {
    case protocol::pkt_type::FT_MSG: {
      FASTT_LOG_DEBUG("Got new msg seq=%u ack=%u ackframe=%u wnd=%u\n",
                      hdr->seq.v, hdr->ack.v, hdr->ackframe, hdr->wnd);
      scheduler.process_seq(hdr->seq);
      if (trx.is_retransmission_or_exceeds_capacity(hdr->seq,
                                                    stats.retransmissions)) {
        msg->free();
        return false;
      }
      if (hdr->ackframe){
        ttx.acknowledge(hdr->ack, ts, hdr->sack);
        ttx.detect_loss(ts);
      }
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
        ttx.acknowledge_sack(sack_payload, ts);
      }
      ttx.detect_loss(ts);
      if (cstate == connection_state::DISCONNECTING && ttx.all_acked())
        cstate = connection_state::DISCONNECTED;
      assert(hdr->wnd == 0);
      msg->free();
      break;
    }
    case protocol::pkt_type::FT_SYN: {
      FASTT_LOG_DEBUG("Got RDY_TO_RCV seq=%u wnd=%u\n", hdr->seq.v, hdr->wnd);
      scheduler.process_seq(hdr->seq);
      if (trx.is_retransmission_or_exceeds_capacity(hdr->seq,
                                                    stats.retransmissions)) {
        msg->free();
        return false;
      }
      auto *pyld =
          msg->data<protocol::ft_init_payload>(sizeof(protocol::ft_header));
      cfg.transport_ports.sport = pyld->sport;
      cfg.transport_ports.dport = pyld->dport;
      ttx.update_budget(hdr->wnd);
      trx.insert(hdr->seq, msg);
      cstate = connection_state::ESTABLISHING;
      break;
    }
    case protocol::pkt_type::FT_SYN_ACK: {
      FASTT_LOG_DEBUG("Got CLR_TO_SD seq=%u ack=%u wnd=%u\n", hdr->seq.v,
                      hdr->ack.v, hdr->wnd);
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
      if (hdr->ackframe){
        ttx.acknowledge(hdr->ack, ts, false);
        ttx.detect_loss(ts);
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
        // retransmissions should be acknowledged
        acknowledge();
        return false;
      }
      ttx.acknowledge(hdr->ack, ts, hdr->sack);
      trx.insert(hdr->seq, msg);
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
    auto *msg = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
    auto now = rte_get_timer_cycles();
    ttx.record_ctrl_pkt(msg, [&](msg_fragment *msg, seq_t seq) {
      auto ackframe = scheduler.ack_pending(trx.get_last_rcvd_in_seq());
      if(ackframe)
        scheduler.ack_callback(trx.get_last_rcvd_in_seq());
      builder.prepare_done_header(msg, seq, trx.get_last_rcvd_in_seq(), ackframe);
    }, now);
    cstate = connection_state::DISCONNECTING;
    pkt_if->consume_pkt(msg, cfg);
  }

  void open_connection(uint16_t rx_flow_sport, uint16_t rx_flow_dport) {
    auto *msg = allocator->alloc_msg_fragment(
        sizeof(protocol::ft_header) + sizeof(protocol::ft_init_payload));
    assert(msg);
    auto *init_payload =
        msg->data<protocol::ft_init_payload>(sizeof(protocol::ft_header));
    init_payload->sport = rx_flow_sport;
    init_payload->dport = rx_flow_dport;
    auto now = rte_get_timer_cycles();
    ttx.record_ctrl_pkt(msg, [&, budget = trx.prepare_wnd_return()](
                                 msg_fragment *msg, seq_t seq) {
      FASTT_LOG_DEBUG("Sent SYN seq=%u wnd=%u flow=%s\n", seq.v, budget,
                      get_flow_tuple().print().c_str());
      builder.prepare_init_header(msg, seq, budget);
    }, now);
    ttx.rearm(rte_get_timer_cycles());
    auto *hdr = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    assert(hdr->type == protocol::FT_SYN);
    FASTT_LOG_DEBUG("Sent SYN seq=%u wnd=%u flow=%s\n", hdr->seq.v, hdr->wnd,
                    get_flow_tuple().print().c_str());
    pkt_if->consume_pkt(msg, cfg);
  }

  void accept_connection() {
    auto *msg = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
    assert(msg);
    auto ack = trx.get_last_rcvd_in_seq();
    auto now = rte_get_timer_cycles();
    ttx.record_ctrl_pkt(msg, [&, budget = trx.prepare_wnd_return()](
                                 msg_fragment *msg, seq_t seq) {
      FASTT_LOG_DEBUG("Sent FT_SYN_ACK seq=%u ack=%u wnd=%u flow=%s\n", seq.v,
                      ack.v, budget, get_flow_tuple().print().c_str());
      auto ackframe = scheduler.ack_pending(ack);
      if(ackframe)
        scheduler.ack_callback(ack);
      builder.prepare_init_ack_header(msg, seq, ack, budget, ackframe);
    }, now);
    ttx.rearm(now);
    pkt_if->consume_pkt(msg, cfg);
    cstate = connection_state::ESTABLISHED;
  }

  bool all_acked() const { return ttx.all_acked(); }

  bool up() { return connection_state::ESTABLISHED == cstate; }

  bool disconnected() { return connection_state::DISCONNECTED == cstate; }

  connection_state get_state() const { return cstate; }

  bool can_recv() {
    return trx.has_buffered_msg_fragments_frags() ||
           cstate != connection_state::ESTABLISHED;
  }

  bool can_send() {
    return ttx.get_current_wnd() > 0 || cstate != connection_state::ESTABLISHED;
  }

  ssize_t send_single(void *buf, size_t size, size_t off) {
    if (!ttx.get_current_wnd())
      return -EAGAIN;
    bool som = off == 0;
    size_t som_len = som ? sizeof(protocol::ft_msg_payload) : 0;
    size_t send_size = std::min<size_t>(size - off, kMaxPayload - som_len);
    auto *mbuf = allocator->alloc_msg_fragment(send_size + som_len);
    if (som)
      mbuf->data<protocol::ft_msg_payload>()->out = size;
    bool eom = off + send_size >= size;
    std::memcpy(mbuf->data<uint8_t>(som_len), buf, send_size);
    auto now = rte_get_timer_cycles();
    auto ctor = [&](msg_fragment *pkt, seq_t seq) {
      auto ack_seq = trx.get_last_rcvd_in_seq();  
      protocol::msg_frame_desc desc{
          .seq = seq,
          .ack = ack_seq,
          .wnd = trx.prepare_wnd_return(),
          .som = som,
          .eom = eom,
          .ack_frame = scheduler.ack_pending(ack_seq),
          .sack = false,
      };
      if(desc.ack_frame)
        scheduler.ack_callback(desc.ack);
      builder.prepare_ft_header(pkt, desc);
    };
    auto inserted = ttx.record_pkt(mbuf, ctor, now);
    // we check the current grant before
    assert(inserted);
    pkt_if->consume_pkt(mbuf, cfg);
    return send_size;
  }

  ssize_t send(msg_hdr &hdr) {
    if (connection_state::ESTABLISHED != cstate)
      return 0;
    auto &off = hdr.off;
    ssize_t sent = 0;
    for (; off < hdr.len;) {
      auto retval =
          send_single(static_cast<uint8_t *>(hdr.buf) + off, hdr.len, off);
      if (retval < 0) {
        sent = sent == 0 ? retval : sent;
        break;
      }
      sent += retval;
      off += retval;
    }
    FASTT_LOG_DEBUG("send iov_len=%lu total=%zd\n", hdr.len, sent);
    return sent;
  }

  ssize_t recv(void *buf, size_t size, size_t &remaining) {
    if (connection_state::ESTABLISHED != cstate)
      return 0;
    auto ret = trx.read(buf, size, remaining);
    if (ret == -EAGAIN)
      return ret;
    FASTT_LOG_DEBUG("recv ret=%zd\n", ret);
    return ret;
  }

  transport_statistics get_stats() {
    auto rt_stats = ttx.get_stats();
    return {rt_stats.retransmitted, rt_stats.acked, stats.sent,
            stats.retransmissions, rt_stats.rtt};
  }

  flow_tuple get_flow_tuple() const {
    flow_tuple ft;
    ft.dip = pkt_if->get_sip();
    ft.dport = builder.dport;
    ft.sport = builder.sport;
    ft.sip = cfg.ip;
    return ft;
  }

private:
  transport_output trx;
  protocol::builder builder;
  transport_config cfg;
  transport_input ttx;
  ack_scheduler scheduler;
  msg_fragment_allocator *allocator;
  P *pkt_if;
  uint64_t rto = get_ticks_ms() * 10;
  connection_state cstate = connection_state::ESTABLISHING;
};
