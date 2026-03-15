#pragma once

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <generic/rte_cycles.h>
#include <sys/types.h>

#include "debug.h"
#include "packet_if.h"
#include "protocol.h"
#include "protocol_util.h"
#include "task/task.h"

#include "sgl.h"
#include "slab_allocator.h"
#include "transport/seq.h"
#include "transport_rxpath.h"
#include "transport/congestion_control.h"
#include "transport_txpath.h"
#include "transport_stats.h"
#include "util.h"

enum class connection_state {
  ESTABLISHING,
  ESTABLISHED,
  DISCONNECTING,
  DISCONNECTED
};

class connection_manager;

template <typename P = packet_if, typename M = connection_manager> class transport {
  friend M;
public:
  static constexpr uint16_t kMaxPayload = 1500 - protocol::defs::kHeaderMTUlen;
  struct {
    uint64_t sent = 0;
    uint64_t retransmissions = 0;
  } stats;

  transport(P *pkt_sink, slab_allocator *sb, M* manager, transport_config cfg,
            uint16_t sport, uint16_t dport)
      : cc(get_ticks_us() * 100), trx(), builder(sport, dport), cfg(cfg), ttx(cc), sb(sb), acb(), manager(manager),
        pkt_if(pkt_sink) {}

  void perform_recovery() {
    ttx.advance_recovery([&](mbuf *pkt) -> bool {
      pkt_if->consume_pkt_mbuf(pkt, cfg);
      return true;
    }, manager->get_current_timer_cycles());
  }

  void check_timeout(uint64_t now) {
    if (cstate == connection_state::DISCONNECTED)
      return;
    if (ttx.all_acked())
      return;
    if (ttx.check_timeout(now)) {
      ttx.rto_retransmit(now);
      perform_recovery();
      ttx.rearm(now);
    }
  }

  bool acknowledge() {
    mbuf_ptr msg = mbuf_take_owner_ship(nullptr);
    bool is_sack = trx.has_holes();
    seq_t ack = trx.get_last_rcvd_in_seq();
    if (is_sack) {
      if (!acb.has_unacked_pkts())
        return false;
      msg = sb->alloc_default_safe(sizeof(protocol::ft_header) +
                                   sizeof(protocol::ft_sack_payload));
      if (!msg)
        return false;
      auto *sack_payload =
          msg->data<protocol::ft_sack_payload>(sizeof(protocol::ft_header));
      trx.pack_sack(sack_payload);
      acb.mark_as_acked(ack);
      FASTT_LOG_DEBUG("Sending SACK of size %u with contiguos ack until %u\n",
                      sack_payload->bit_map_len, ack.v);
    } else {
      if (!acb.has_unacked_pkts())
        return false;
      msg = sb->alloc_default_safe(sizeof(protocol::ft_header));
      if (!msg)
        return false;
      acb.mark_as_acked(ack);
      FASTT_LOG_DEBUG("Sending ACK ack=%u\n", ack.v);
    }
    if (trx.seen_done)
      cstate = connection_state::DISCONNECTED;

    builder.prepare_ack_pkt(msg.get(), ack, is_sack);
    pkt_if->consume_pkt_mbuf(msg.get(), cfg);
    return true;
  }

  bool check_pkt(mbuf *pkt, seq_t seq) {
    if (trx.is_retransmission(seq)) {
      ++stats.retransmissions;
      if (!acb.pending_dup_acks)
        acb.add_dump_ack();
      mbuf_free(pkt);
      return false;
    } else if (trx.exceeds_capacity(seq)) {
      mbuf_free(pkt);
      return false;
    }
    return true;
  }

  bool process_pkt(mbuf *msg) {
    auto *hdr = msg->data<const protocol::ft_header>();
    auto ts = manager->get_current_timer_cycles();
    switch (hdr->type) {
    case protocol::pkt_type::FT_MSG: {
      FASTT_LOG_DEBUG("Got new msg seq=%u ack=%u ackframe=%u wnd=%u\n",
                      hdr->seq.v, hdr->ack.v, hdr->ackframe, hdr->crd);
      if (!check_pkt(msg, hdr->seq))
        return false;
      if (hdr->ackframe) {
        ttx.acknowledge(hdr->ack, ts);
        ttx.detect_loss(ts);
      }
      if (hdr->crd)
        ttx.update_budget(hdr->crd);
      trx.insert(hdr->seq, msg, acb);
      break;
    }
    case protocol::pkt_type::FT_ACK: {
      FASTT_LOG_DEBUG("Got ACK ack=%u sack=%u\n", hdr->ack.v, hdr->sack);
      ttx.acknowledge(hdr->ack, ts);
      if (hdr->sack) {
        auto *sack_payload =
            msg->data<protocol::ft_sack_payload>(sizeof(protocol::ft_header));
        ttx.acknowledge_sack(sack_payload, ts);
      }
      ttx.detect_loss(ts);
      if (cstate == connection_state::DISCONNECTING && ttx.all_acked())
        cstate = connection_state::DISCONNECTED;
      assert(hdr->crd == 0);
      mbuf_free(msg);
      break;
    }
    case protocol::pkt_type::FT_SYN: {
      FASTT_LOG_DEBUG("Got RDY_TO_RCV seq=%u wnd=%u\n", hdr->seq.v, hdr->crd);
      if (!check_pkt(msg, hdr->seq))
        return false;
      auto *pyld =
          msg->data<protocol::ft_init_payload>(sizeof(protocol::ft_header));
      cfg.transport_ports.sport = pyld->sport;
      cfg.transport_ports.dport = pyld->dport;
      ttx.update_budget(hdr->crd);
      trx.insert(hdr->seq, msg, acb);
      cstate = connection_state::ESTABLISHING;
      break;
    }
    case protocol::pkt_type::FT_SYN_ACK: {
      FASTT_LOG_DEBUG("Got CLR_TO_SD seq=%u ack=%u wnd=%u\n", hdr->seq.v,
                      hdr->ack.v, hdr->crd);
      if (!check_pkt(msg, hdr->seq))
        return false;
      ttx.acknowledge(hdr->ack, ts);
      assert(hdr->crd > 0);
      ttx.update_budget(hdr->crd);
      trx.insert(hdr->seq, msg, acb);
      cstate = connection_state::ESTABLISHED;
      break;
    }
    case protocol::pkt_type::FT_CRD_UPDATE: {
      FASTT_LOG_DEBUG("Got WND_RET seq=%u wnd=%u\n", hdr->seq.v, hdr->crd);
      if (!check_pkt(msg, hdr->seq))
        return false;
      if (hdr->ackframe) {
        ttx.acknowledge(hdr->ack, ts);
        ttx.detect_loss(ts);
      }
      ttx.update_budget(hdr->crd);
      trx.insert(hdr->seq, msg, acb);
      break;
    }
    case protocol::pkt_type::FT_DONE: {
      FASTT_LOG_DEBUG("Got DONE seq=%u ack=%u\n", hdr->seq.v, hdr->ack.v);
      if (trx.is_retransmission(hdr->seq)) {
        ++acb.pending_dup_acks;
        acknowledge();
        mbuf_free(msg);
        return false;
      } else if (trx.exceeds_capacity(hdr->seq)) {
        mbuf_free(msg);
        return false;
      }
      ttx.acknowledge(hdr->ack, ts);
      // if the connection is done and only the last packet if missing proceed
      // otherwise drop
      assert(ttx.all_acked());
      cstate = connection_state::DISCONNECTING;
      trx.insert(hdr->seq, msg, acb);
      break;
    }
    default:
      mbuf_free(msg);
      break;
    }
    return true;
  }

  void close_connection() {
    // just send for now  
    // we wait till everything is acked anyway because of RPC pattern
    auto now = manager->get_current_timer_cycles();
    auto *pkt = sb->alloc_default(sizeof(protocol::ft_header));
    ttx.record_ctrl_pkt(
        pkt,
        [&](mbuf *msg, seq_t seq) {
          auto ackframe = acb.has_unacked_pkts();
          if (ackframe)
            acb.mark_as_acked(trx.get_last_rcvd_in_seq());
          builder.prepare_done_header(msg, seq, trx.get_last_rcvd_in_seq(),
                                      ackframe);
        },
        now);
    cstate = connection_state::DISCONNECTING;
    pkt_if->consume_pkt_mbuf(pkt, cfg);
  }

  void open_connection(uint16_t rx_flow_sport, uint16_t rx_flow_dport) {
    auto *msg = sb->alloc_default(sizeof(protocol::ft_header) +
                                  sizeof(protocol::ft_init_payload));

    assert(msg);
    auto *init_payload =
        msg->data<protocol::ft_init_payload>(sizeof(protocol::ft_header));
    init_payload->sport = rx_flow_sport;
    init_payload->dport = rx_flow_dport;
    auto now = manager->get_current_timer_cycles();
    ttx.record_ctrl_pkt(
        msg,
        [&, budget = trx.get_available_wnd()](mbuf *msg, seq_t seq) {
          FASTT_LOG_DEBUG("Sent SYN seq=%u wnd=%u flow=%s\n", seq.v, budget,
                          get_flow_tuple().print().c_str());
          builder.prepare_init_header(msg, seq, budget);
        },
        now);
    ttx.rearm(manager->get_current_timer_cycles());
    auto *hdr = msg->data<protocol::ft_header>();
    assert(hdr->type == protocol::FT_SYN);
    FASTT_LOG_DEBUG("Sent SYN seq=%u wnd=%u flow=%s\n", hdr->seq.v, hdr->crd,
                    get_flow_tuple().print().c_str());
    pkt_if->consume_pkt_mbuf(msg, cfg);
  }

  void accept_connection() {
    auto *pkt = sb->alloc_default(sizeof(protocol::ft_header));
    auto ack = trx.get_last_rcvd_in_seq();
    auto now = manager->get_current_timer_cycles();
    ttx.record_ctrl_pkt(
        pkt,
        [&, budget = trx.get_available_wnd()](mbuf *msg, seq_t seq) {
          FASTT_LOG_DEBUG("Sent FT_SYN_ACK seq=%u ack=%u wnd=%u flow=%s\n",
                          seq.v, ack.v, budget,
                          get_flow_tuple().print().c_str());
          auto ackframe = acb.has_unacked_pkts();
          if (ackframe)
            acb.mark_as_acked(ack);
          builder.prepare_init_ack_header(msg, seq, ack, budget, ackframe);
        },
        now);
    ttx.rearm(now);
    pkt_if->consume_pkt_mbuf(pkt, cfg);
    cstate = connection_state::ESTABLISHED;
  }

  bool all_acked() const { return ttx.all_acked(); }

  bool up() { return connection_state::ESTABLISHED == cstate; }

  bool disconnected() { return connection_state::DISCONNECTED == cstate; }

  connection_state get_state() const { return cstate; }

  bool can_recv() {
    return trx.has_buffered_mbufs_frags();
  }

  bool can_send() {
    return (ttx.get_current_wnd() > 0);
  }

  hdr_histogram* get_hist(){
      return cc.hist; 
  }

  ssize_t send_single_seg(sgl &msgl) {
    if (!ttx.can_transmit())
      return -EAGAIN;
    auto now = manager->get_current_timer_cycles();
    auto pkt = std::move(msgl).take_head();
    auto send_size = pkt->data_len;
    auto ctor = [&](mbuf_ptr &pkt, seq_t seq) {
      auto ack_seq = trx.get_last_rcvd_in_seq();
      protocol::msg_frame_desc desc{
          .seq = seq,
          .ack = ack_seq,
          .crd = 0,
          .eom = msgl.empty(),
          .ack_frame = acb.has_unacked_pkts() && !trx.has_holes(),
          .sack = false,
      };
      if (desc.ack_frame)
        acb.mark_as_acked(desc.ack);
      FASTT_LOG_DEBUG("Piggbacked: %d %u\n", desc.ack_frame, desc.ack.v);
      builder.prepare_ft_header(pkt.get(), desc);
    };
    auto out_pkt = pkt.get();
    auto inserted = ttx.record_pkt(std::move(pkt), ctor, now);
    // we check the current grant before
    assert(inserted);
    pkt_if->consume_pkt_mbuf(out_pkt, cfg);
    return send_size;
  }

  ssize_t send_sgl(sgl &msgl) {
    if (connection_state::ESTABLISHED != cstate)
      return 0;
    ssize_t sent = 0;
    for (; !msgl.empty();) {
      auto retval = send_single_seg(msgl);
      if (retval < 0) {
        sent = sent == 0 ? retval : sent;
        break;
      }
      sent += retval;
    }
    FASTT_LOG_DEBUG("send len=%lu total=%zd\n", msgl.size, sent);
    return sent;
  }

  ssize_t recv(sgl &msgl) {
    if (connection_state::ESTABLISHED != cstate)
      return 0;
    auto ret = trx.read(msgl);
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
  swift cc;
  transport_rxpath trx;
  protocol::builder builder;
  transport_config cfg;
  transport_txpath ttx;
  slab_allocator *sb;
  ack_cb acb;
  M* manager;
  P *pkt_if;
  connection_state cstate = connection_state::ESTABLISHING;
public:
  std::optional<concurrency::coro_handle> coro;
  list_hook link;
  list_hook ready;
};
