#pragma once

#include <cassert>
#include <cstdint>
#include <deque>

#include "debug.h"
#include "filter.h"
#include "protocol.h"
#include "slab_allocator.h"
#include "transport/congestion_control.h"
#include "transport/seq.h"
#include "transport/transport_rxpath.h"
#include "util.h"

struct rack {
  static constexpr uint64_t kMinRTT = 30;
  static constexpr uint64_t kDefaultReoMult = 1;
  static constexpr uint16_t kDupThresh = 3;
  static bool send_after(uint64_t t1, seq_t seq1, uint64_t t2, seq_t seq2) {
    if (t1 > t2)
      return true;
    else if (t1 == t2 && seq1 > seq2)
      return true;
    else
      return false;
  }

  bool valid_rtt(uint64_t now, uint64_t seg_xmit_ts, bool retransmitted) {
    if (retransmitted)
      return now - seg_xmit_ts >= min_rtt;
    return true;
  }

  void update(uint64_t seg_xmit_ts, seq_t seq) {
    if (send_after(seg_xmit_ts, seq, xmit_ts, end_seq)) {
      xmit_ts = seg_xmit_ts;
      end_seq = seq;
    }
  }

  void detect_reordering(seq_t seq, bool retransmitted) {
    if (seq > fack)
      fack = seq;
    else if (seq < fack && !retransmitted)
      reordering_seen = true;
  }

  void update_reo_wnd(bool in_recovery, uint64_t segs_sacked, uint64_t srtt) {
    if (!reordering_seen) {
      if (in_recovery)
        reo_wnd = 0;
      else if (segs_sacked >= kDupThresh)
        reo_wnd = 0;
      return;
    }
    reo_wnd = std::min(kDefaultReoMult * min_rtt / 4, srtt);
  }

  rack(seq_t end_seq) : end_seq(end_seq), fack(end_seq) {}

  uint64_t min_rtt{kMinRTT * get_ticks_us()}, rtt = 0;
  uint64_t xmit_ts = 0, reo_wnd = 0;
  seq_t end_seq, fack;
  uint64_t dup_ack_cnt = 0;
  bool reordering_seen = false;

  seq_t high_data;
  bool in_fast_recovery = false, in_rto_recovery = false;
};

struct sender_entry {
  list_hook link;
  mbuf_ptr packet;
  uint64_t xmit_ts = 0;
  seq_t seq;
  uint16_t crd = 0;
  bool sacked : 4;
  bool retransmitted : 2;
  bool queued : 2;
  sender_entry(mbuf_ptr &&packet, uint64_t now, seq_t seq, uint16_t crd,
               bool retransmitted)
      : packet(std::move(packet)), xmit_ts(now), seq(seq), crd(crd),
        sacked(false), retransmitted(retransmitted), queued(false) {}

  sender_entry(const sender_entry &) = delete;

  ~sender_entry() {
    if (link.is_linked())
      link.unlink();
  }
};

class transport_txpath {
public:
  struct statistics {
    seq_t acked;
    uint64_t retransmitted, rtt, sent;
    statistics() : acked(0), retransmitted(0) {}
  };
  transport_txpath(swift &cc, seq_t seq = {0})
      : cc(cc), seq(seq), least_unacked_pkt(seq), rtt(), timeout(),
        rck(seq - 1) {}

  void rto_retransmit(uint64_t ts) {
    uint64_t lost = 0;
    // we dont renege
    for (auto it = xmit_list.begin(), end = xmit_list.end(); it != end;) {
      auto &entry = *it;
      ++it;
      if (entry.seq == least_unacked_pkt ||
          ts - entry.xmit_ts >= rck.rtt + rck.reo_wnd) {
        FASTT_LOG_DEBUG("Detected loss %u\n", entry.seq.v);
        assert(entry.link.is_linked());
        entry.link.unlink();
        assert(inflight_pkts > 0);
        --inflight_pkts;
        entry.queued = true;
        retransmission_queue.push_back(entry);
        ++lost;
      }
    }

    if(lost && !rck.in_rto_recovery){
        rck.in_rto_recovery = true;
        rck.in_fast_recovery = false;
        rck.high_data = seq;
        cc.on_retransmission_timeout(lost, rtt, ts);
    }
    rearm(ts);
  }

  void detect_loss(uint64_t now) {
    uint64_t lost = 0;
    rck.update_reo_wnd(!retransmission_queue.empty(), segs_sacked, rtt);
    for (auto it = xmit_list.begin(), end = xmit_list.end(); it != end;) {
      auto &entry = *it;
      ++it;
      if (!rack::send_after(rck.xmit_ts, rck.end_seq, entry.xmit_ts, entry.seq))
        break;
      if (now >= entry.xmit_ts + rck.rtt + rck.reo_wnd) {
        FASTT_LOG_DEBUG("Detected loss %u\n", entry.seq.v);
        assert(entry.link.is_linked());
        entry.link.unlink();
        assert(inflight_pkts > 0);
        --inflight_pkts;
        entry.queued = true;
        retransmission_queue.push_back(entry);
        ++lost;
      }
    }
    if (lost && !rck.in_fast_recovery && !rck.in_rto_recovery) {
      rck.in_fast_recovery = true;
      rck.high_data = seq;
      cc.on_fast_recovery(now, rtt);
    }

  }

  unsigned get_current_wnd() const { return budget; }

  bool can_transmit() {
    return budget > 0 && (cc.space(inflight_pkts) > 0 || cc.rate_limited());
  }

  bool check_timeout(uint64_t now) {
    if (now > timeout)
      return true;
    return false;
  }

  void rearm(uint64_t ts) { timeout = ts + rto; }

  void cleanup_acked_pkts(seq_t seq, uint64_t ts) {
    uint64_t cumulative_rtt = ~0ull;
    uint64_t acked = 0;
    while (!unacked.empty() && unacked.front().seq <= seq) {
      auto &desc = unacked.front();
      assert(ts >= desc.xmit_ts);
      auto ack_rtt = ts - desc.xmit_ts;
      if (!desc.sacked) {
        if (rck.valid_rtt(ts, desc.xmit_ts, desc.retransmitted)) {
          rck.update(desc.xmit_ts, desc.seq);
          cumulative_rtt = std::min<uint64_t>(ack_rtt, cumulative_rtt);
        }
        assert(desc.link.is_linked());
        rck.detect_reordering(desc.seq, desc.retransmitted);
      }
      if (!desc.sacked && !desc.queued) {
        assert(inflight_pkts > 0);
        --inflight_pkts;
      }
      if (desc.sacked)
        --segs_sacked;
      budget += desc.crd;
      ++acked;
      unacked.pop_front();
    }
    if (cumulative_rtt != ~0ull) {
      update_srtt(cumulative_rtt);
      rck.rtt = cumulative_rtt;
    }

    assert(budget <= transport_rxpath::kMaxGrantSize);
    cc.on_ack(acked, ts, rtt, rck.rtt);
  }

  template <typename F>
  void record_ctrl_pkt(mbuf *pkt, F &&ctor, uint64_t now) {
    if (all_acked())
      rearm(now);
    ctor(pkt, seq);
    pkt->xmit = false;
    ++inflight_pkts;
    ++xmitted;
    unacked.emplace_back(mbuf_take_owner_ship(pkt), now, seq++, 0, false);
    xmit_list.push_back(unacked.back());
  }

  template <typename F>
  bool record_pkt(mbuf_ptr &&pkt, F &&ctor, uint64_t now) {
    if (budget == 0)
      return false;
    if (all_acked())
      rearm(now);
    --budget;
    ctor(pkt, seq);
    pkt->xmit = false;
    ++inflight_pkts;
    ++xmitted;
    unacked.emplace_back(std::move(pkt), now, seq++, 1, false);
    xmit_list.push_back(unacked.back());
    assert(timeout >= now);
    return true;
  }

  template <typename F> void advance_recovery(F &&f, uint64_t now) {
    if (retransmission_queue.empty())
      return;
    auto sz = retransmission_queue.size();
    while (sz-- > 0) {
      auto &desc = retransmission_queue.front();
      assert(desc.queued);
      if (!cc.space(inflight_pkts))
        break;
      if (!f(desc.packet.get()))
        break;

      ++stats.retransmitted;
      // inc reference count
      // in total we have n + 1 where n is the number of transmissions
      // entry->msg n reduction because of cleanup
      desc.xmit_ts = now;
      desc.retransmitted = true;
      desc.packet->xmit = false;
      desc.queued = false;
      desc.link.unlink();
      ++inflight_pkts;
      xmit_list.push_back(desc);
    }

    if(rck.in_rto_recovery && sz == 0)
        rearm(now);
  }

  void acknowledge(seq_t seq, uint64_t ts) {
    if (seq < least_unacked_pkt)
      return;
    stats.acked = seq;
    cleanup_acked_pkts(seq, ts);
    rearm(ts);
    least_unacked_pkt = seq + 1;
    if ((rck.in_fast_recovery || rck.in_rto_recovery) && seq > rck.high_data){
      rck.in_fast_recovery = false;
      rck.in_rto_recovery = false;
    }

    assert(timeout >= ts || xmit_list.empty());
  }

  void acknowledge_sack(protocol::ft_sack_payload *payload,
                        seq_t cumulative_ack, uint64_t ts) {
    assert(payload->bit_map_len > 0);
    assert(unacked.front().seq == least_unacked_pkt);
    FASTT_LOG_DEBUG("Received SACK of length %u\n", payload->bit_map_len);
    auto it = unacked.begin();
    // might happen in case of reordering
    auto cumulative_ack_in_pkt = cumulative_ack;
    auto last_acked = least_unacked_pkt - 1;
    assert(cumulative_ack <= last_acked);
    auto i = last_acked - cumulative_ack;
    uint64_t sack_rtt = ~0ull;
    for (; i < payload->bit_map_len; ++i) {
      auto ind = get_bit_indices_64(i);
      auto val = payload->bit_map[ind.first] & (1ull << ind.second);
      auto &desc = *it;
      ++it;
      assert(cumulative_ack_in_pkt + i + 1 == desc.seq);
      if (!val)
        continue;
      if (!desc.sacked) {
        ++segs_sacked;
        assert(ts >= desc.xmit_ts);
        auto ack_rtt = ts - desc.xmit_ts;
        if (rck.valid_rtt(ts, desc.xmit_ts, desc.retransmitted)) {
          rck.update(desc.xmit_ts, desc.seq);
          sack_rtt = std::min<uint64_t>(ack_rtt, sack_rtt);
        }

        desc.sacked = true;
        assert(desc.link.is_linked());
        desc.link.unlink();
        if (!desc.queued)
          --inflight_pkts;
        rck.detect_reordering(desc.seq, desc.retransmitted);
      }
    }

    if (sack_rtt != ~0ull) {
      update_srtt(sack_rtt);
      rck.rtt = sack_rtt;
    }
  }

  auto size() { return unacked.size(); }

  void update_srtt(uint64_t est) {
    if (rtt == 0)
      rtt = est;
    else
      rtt = filter::exp_filter(rtt, est);
    rto = std::max(2 * rtt, default_rto);
    stats.rtt = rtt;
  }

  seq_t get_seq() const { return seq; }

  uint64_t get_srtt() const { return rtt; }

  bool all_acked() const { return least_unacked_pkt == seq; }

  void update_budget(uint16_t granted) {
    budget += granted;
    FASTT_LOG_DEBUG("Got new capacity %u\n", budget);
  }

  statistics get_stats() {
    statistics out = stats;
    out.rtt /= get_ticks_us();
    out.sent = xmitted;
    return out;
  }

private:
  static constexpr uint64_t kMinRTT = 10;
  statistics stats;

  swift &cc;
  uint64_t inflight_pkts = 0;

  std::deque<sender_entry> unacked;
  intrusive_list_t<sender_entry> retransmission_queue;
  intrusive_list_t<sender_entry> xmit_list;

  uint32_t budget = 0;
  seq_t seq;
  seq_t least_unacked_pkt;

  uint64_t rtt = 0;
  uint64_t xmitted = 0;
  uint64_t segs_sacked = 0;
  const uint64_t default_rto = get_ticks_ms() * 10;
  uint64_t rto = default_rto;
  uint64_t timeout;
  rack rck;
};
