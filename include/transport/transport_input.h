#pragma once

#include <cassert>
#include <cstdint>
#include <deque>
#include <generic/rte_cycles.h>
#include <message.h>
#include <rte_cycles.h>

#include "congestion_control.h"
#include "debug.h"
#include "filter.h"
#include "message.h"
#include "protocol.h"
#include "util.h"

static constexpr uint64_t min_seq = 1;

struct sender_entry {
  message *packet;
  uint64_t seq;
  bool sacked : 4;
  bool retransmitted : 4;
  sender_entry() : packet(nullptr), seq(0), retransmitted(false) {}
  sender_entry(message *packet, uint64_t seq, bool retransmitted)
      : packet(packet), seq(seq), sacked(false), retransmitted(retransmitted) {}

  bool requires_retry(uint64_t now, uint64_t rto) {
    return now > *packet->get_ts() + rto;
  }
  message *get() { return packet; }

  sender_entry(const sender_entry &) = delete;
};

class transport_input {
public:
  struct statistics {
    uint64_t acked, retransmitted, rtt;
    statistics() : acked(0), retransmitted(0) {}
  };
  transport_input(uint32_t budget = 1)
      : cc(16, 80), budget(budget), seq(min_seq), rtt() {}

  unsigned get_current_wnd() const {
    return budget;
  }

  bool check_timeout(uint64_t now) {
    if (now > timeout)
      return true;
    return false;
  }

  void rearm(uint64_t now) { timeout = now + rto; }

  uint64_t cleanup_acked_pkts(uint64_t seq, uint64_t ts) {
    uint64_t burst_rtt = 0;
    while (!unacked.empty() && unacked.front().seq < seq) {
      auto &desc = unacked.front();
      assert(desc.packet);
      rte_pktmbuf_free(desc.packet);
      unacked.pop_front();
    }

    auto &srtt_desc = unacked.front();
    update_srtt(&srtt_desc, ts);
    rte_pktmbuf_free(srtt_desc.packet);
    unacked.pop_front();
    return burst_rtt;
  }

  template <typename F> bool record_pkt(message *msg, F &&ctor) {
    if (budget == 0)
      return false;
    --budget;
    ctor(msg, seq);
    msg->inc_refcnt();
    *msg->get_ts() = 0;
    unacked.emplace_back(msg, seq++, false);
    FASTT_LOG_DEBUG("Enqueue pkt with %lu new budget %u\n", seq - 1, budget);
    if (all_acked())
      timeout = rte_get_timer_cycles() + rto;

    return true;
  }

  template <typename F> void probe_retransmit(F &&cb) {
    for (auto &entry : unacked) {
      auto *msg = entry.packet;
      if (*msg->get_ts() == 0)
        continue;
      if (entry.sacked)
        continue;
      FASTT_LOG_DEBUG("Retransmitting packet: %lu\n", entry.seq);
      prepare_retransmit(&entry);
      cb(msg);
    }
  }

  void prepare_retransmit(sender_entry *entry) {
    ++stats.retransmitted;
    // inc reference count
    // in total we have n + 1 where n is the number of transmissions
    // entry->msg n reduction because of cleanup
    entry->packet->inc_refcnt();
    *entry->packet->get_ts() = 0;
    entry->retransmitted = true;
  }

  void acknowledge(uint64_t seq, uint16_t budget, uint64_t ts, bool is_sack) {
    if (seq < least_unacked_pkt)
      return;
    stats.acked = seq;
    if (!is_sack) {
      update_budget(budget, seq);
      timeout = rte_get_timer_cycles() + rto;
    }
    cleanup_acked_pkts(seq, ts);
    least_unacked_pkt = seq + 1;
  }

  template <typename F>
  void acknowledge_sack(protocol::ft_sack_payload *payload, uint64_t budget,
                        uint64_t ts, F &&retransmit_cb) {
    sender_entry *largest_acked = nullptr;
    uint64_t largest_acked_seq = 0;
    assert(payload->bit_map_len > 0);
    assert(payload->bit_map_len <= unacked.size());
    assert(unacked.front().seq == least_unacked_pkt);
    auto it = unacked.begin();
    for (auto i = 0u; i < payload->bit_map_len; ++i) {
      auto ind = get_bit_indices_64(i);
      auto val = payload->bit_map[ind.first] & (1ull << ind.second);
      auto &desc = *it;

      if (!val) {
        prepare_retransmit(&desc);
        retransmit_cb(desc.packet);
      } else if (!desc.sacked) {
        /* we want the largest seq not acked yet */
        largest_acked = &(*it);
        largest_acked_seq = it->seq;
        desc.sacked = true;
      }
      ++it;
    }
    timeout = rte_get_timer_cycles() + rto;
    FASTT_LOG_DEBUG("Largest set seq num %lu\n", largest_acked);
    if (largest_acked) {
      update_srtt(largest_acked, ts);
      update_budget(budget, largest_acked_seq);
    }
  }

  auto size() { return unacked.size(); }
  void update_srtt(sender_entry *entry, uint64_t now) {
    auto &desc = *entry;
    if (desc.retransmitted)
      return;
    if (rtt == 0)
      rtt = now - *desc.packet->get_ts();
    else
      rtt = filter::exp_filter(rtt, now - *desc.packet->get_ts());
    stats.rtt = rtt;
  }

  uint64_t get_seq() const { return seq; }
  uint64_t get_srtt() const { return rtt; }

  bool all_acked() const { return least_unacked_pkt == seq; }

  void update_budget(uint16_t granted, uint64_t ack) {
    budget = (granted - (seq - ack - 1));
    FASTT_LOG_DEBUG("Got new capacity %u\n", budget);
  }

  const statistics &get_stats() const { return stats; }

private:
  swift cc;
  statistics stats;
  std::deque<sender_entry> unacked;
  uint32_t budget;
  uint64_t seq;
  uint64_t least_unacked_pkt = min_seq;
  uint64_t rtt;
  uint64_t rto = get_ticks_ms() * 5;
  uint64_t timeout;
};
