#pragma once

#include <cassert>
#include <cstdint>
#include <deque>
#include <generic/rte_cycles.h>
#include <msg_fragment.h>
#include <rte_cycles.h>

#include "debug.h"
#include "filter.h"
#include "msg_fragment.h"
#include "protocol.h"
#include "transport/seq.h"
#include "util.h"

struct rack {
  static constexpr uint64_t kMinRTT = 30;
  static bool send_after(uint64_t t1, seq_t seq1, uint64_t t2, seq_t seq2) {
    if (t1 > t2)
      return true;
    else if (t1 == t2 && seq1 > seq2)
      return true;
    else
      return false;
  }

  bool update(uint64_t seg_xmit_ts, seq_t seq, uint64_t now,
              bool retransmitted) {
    if (retransmitted && now - xmit_ts < min_rtt)
      return false;
    if (send_after(seg_xmit_ts, seq, xmit_ts, end_seq)) {
      xmit_ts = seg_xmit_ts;
      ack_ts = now;
      end_seq = seq;
    }
    return true;
  }

  uint64_t min_rtt{kMinRTT * get_ticks_us()}, rtt;
  uint64_t xmit_ts = 0, ack_ts;
  seq_t end_seq{~0u};
};

struct sender_entry {
  list_hook link;
  msg_fragment *packet; // this is bad, but ok for now
  uint64_t xmit_ts = 0;
  seq_t seq;
  bool sacked : 4;
  bool retransmitted : 4;
  sender_entry() : packet(nullptr), seq(0), retransmitted(false) {}
  sender_entry(msg_fragment *packet, uint64_t now, seq_t seq,
               bool retransmitted)
      : packet(packet), xmit_ts(now), seq(seq), sacked(false),
        retransmitted(retransmitted) {}

  bool requires_retry(uint64_t now, uint64_t rto) {
    return now > *packet->get_ts() + rto;
  }
  msg_fragment *get() { return packet; }

  sender_entry(const sender_entry &) = delete;

  ~sender_entry() {
    if (link.is_linked())
      link.unlink();
    assert(packet != nullptr);
    packet->free();
    packet = nullptr;
  }
};

class transport_input {
public:
  struct statistics {
    seq_t acked;
    uint64_t retransmitted, rtt;
    statistics() : acked(0), retransmitted(0) {}
  };
  transport_input() : rtt(), timeout() {}

  void rto_retransmit(uint64_t ts){
      for(auto it = xmit_list.begin(), end = xmit_list.end(); it != end;){
          auto &entry = *it;
          ++it;
          if(*entry.packet->get_ts() == 0)
              break;
          if(entry.seq == least_unacked_pkt || ts - entry.xmit_ts >= rck.rtt){
              entry.link.unlink();
              retransmission_queue.push_back(entry);
          }
      }
  }

  void detect_loss(uint64_t now) {
    for (auto it = xmit_list.begin(), end = xmit_list.end(); it != end;) {
      auto &entry = *it;
      ++it;
      if (!rack::send_after(rck.xmit_ts, rck.end_seq, entry.xmit_ts, entry.seq))
        break;
      if (now >= entry.xmit_ts + rck.rtt) {
        assert(entry.link.is_linked());  
        entry.link.unlink();
        retransmission_queue.push_back(entry);
      }
    }
  }

  unsigned get_current_wnd() const { return budget; }

  bool check_timeout(uint64_t now) {
    if (now > timeout)
      return true;
    return false;
  }

  void rearm(uint64_t now) { timeout = now + rto; }

  void cleanup_acked_pkts(seq_t seq, uint64_t ts) {
    uint64_t cumulative_rtt = ~0ull;
    while (!unacked.empty() && unacked.front().seq <= seq) {
      auto &desc = unacked.front();
      auto ack_rtt = ts - desc.xmit_ts;
      if (!desc.sacked) {
        if (rck.update(desc.xmit_ts, desc.seq, ts, desc.retransmitted))
          cumulative_rtt = std::min<uint64_t>(ack_rtt, cumulative_rtt);
        assert(desc.link.is_linked());
        inflight -= desc.packet->pkt_len - protocol::defs::kuserDataOffset;
      }
      unacked.pop_front();
    }

    if (cumulative_rtt != ~0ull) {
      update_srtt(cumulative_rtt);
      rck.rtt = cumulative_rtt;
    }
  }

  template <typename F>
  void record_ctrl_pkt(msg_fragment *msg, F &&ctor, uint64_t now) {
    if (all_acked())
      rearm(now);
    ctor(msg, seq);
    msg->inc_refcnt();
    *msg->get_ts() = 0;
    unacked.emplace_back(msg, now, seq++, false);
    xmit_list.push_back(unacked.back());
  }

  template <typename F>
  bool record_pkt(msg_fragment *msg, F &&ctor, uint64_t now) {
    if (budget == 0)
      return false;
    if (all_acked())
      rearm(now);
    --budget;
    ctor(msg, seq);
    inflight += msg->pkt_len;
    msg->inc_refcnt();
    *msg->get_ts() = 0;
    unacked.emplace_back(msg, now, seq++, false);
    xmit_list.push_back(unacked.back());
    FASTT_LOG_DEBUG("Enqueue pkt with %u new budget %u\n", (seq - 1).v, budget);
    return true;
  }

  template <typename F> void advance_recovery(F &&f) {
    if (retransmission_queue.empty())
      return;
    auto sz = retransmission_queue.size();
    auto now = rte_get_timer_cycles();
    while (sz-- > 0) {
      auto &desc = retransmission_queue.front();
      prepare_retransmit(&desc, now);
      f(desc.packet);
    }
  }

  void prepare_retransmit(sender_entry *entry, uint64_t ts) {
    ++stats.retransmitted;
    // inc reference count
    // in total we have n + 1 where n is the number of transmissions
    // entry->msg n reduction because of cleanup
    entry->xmit_ts = ts;
    entry->packet->inc_refcnt();
    *entry->packet->get_ts() = 0;
    entry->retransmitted = true;
    entry->link.unlink();
    xmit_list.push_back(*entry);
  }

  void acknowledge(seq_t seq, uint64_t ts, bool is_sack) {
    if (seq < least_unacked_pkt)
      return;
    stats.acked = seq;
    cleanup_acked_pkts(seq, ts);
    if (!is_sack)
      timeout = ts + rto;
    least_unacked_pkt = seq + 1;
  }

  void acknowledge_sack(protocol::ft_sack_payload *payload, uint64_t ts) {
    assert(payload->bit_map_len > 0);
    assert(payload->bit_map_len <= unacked.size());
    assert(unacked.front().seq == least_unacked_pkt);
    auto it = unacked.begin();
    uint64_t sack_rtt = ~0ull;
    for (auto i = 0u; i < payload->bit_map_len; ++i) {
      auto ind = get_bit_indices_64(i);
      auto val = payload->bit_map[ind.first] & (1ull << ind.second);
      auto &desc = *it;
      ++it;
      if (!val)
        continue;
      if (!desc.sacked) {
        auto ack_rtt = ts - desc.xmit_ts;
        if (rck.update(desc.xmit_ts, desc.seq, ts, desc.retransmitted))
          sack_rtt = std::min<uint64_t>(sack_rtt, ack_rtt);

        inflight -= desc.packet->pkt_len - protocol::defs::kuserDataOffset;
        desc.sacked = true;
        assert(desc.link.is_linked());
        desc.link.unlink();
      }
    }
    timeout = rte_get_timer_cycles() + rto;
    FASTT_LOG_DEBUG("Largest set seq num %u\n", largest_acked->seq.v);

    if (sack_rtt != ~0ull){
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
    stats.rtt = rtt;
  }

  seq_t get_seq() const { return seq; }
  uint64_t get_srtt() const { return rtt; }

  bool all_acked() const { return least_unacked_pkt == seq; }

  void update_budget(uint16_t granted) {
    budget += granted;
    FASTT_LOG_DEBUG("Got new capacity %u\n", budget);
  }

  const statistics &get_stats() {
    stats.rtt /= get_ticks_us();
    return stats;
  }

private:
  static constexpr uint64_t kMinRTT = 10;
  statistics stats;
  intrusive_list_t<sender_entry> retransmission_queue;
  intrusive_list_t<sender_entry> xmit_list;
  std::deque<sender_entry> unacked;
  uint32_t budget = 0;
  seq_t seq{0};
  seq_t least_unacked_pkt{0};
  uint64_t inflight = 0;
  uint64_t rtt = 0;
  uint64_t rto = get_ticks_ms() * 10;
  uint64_t timeout;
  rack rck;
};
