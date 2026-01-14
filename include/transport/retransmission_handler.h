#pragma once
#include <concepts>
#include <cstdint>
#include <deque>
#include <message.h>
#include <rte_cycles.h>
#include <tuple>

#include "message.h"
#include "transport/indexable_queue.h"

static constexpr uint64_t min_seq = 1;

static __inline std::pair<uint64_t, uint64_t>
estimate_timeout(uint64_t rtt, uint64_t rtt_dv, uint64_t measured) {
  static constexpr uint64_t w1 = 1, w2 = 7, shift = 3;
  auto nrtt = (w1 * measured + w2 * rtt) >> shift;
  auto diff = measured > rtt ? measured - rtt : rtt - measured;
  auto nrtt_dv = (w1 * diff + w2 * rtt_dv) >> shift;
  return {nrtt, nrtt_dv};
}

struct sender_entry {
  message *packet;
  uint64_t seq;
  bool retransmitted;
  sender_entry() : packet(nullptr), seq(0), retransmitted(false) {}
  sender_entry(message *packet, uint64_t seq, bool retransmitted)
      : packet(packet), seq(seq), retransmitted(retransmitted) {}

  bool requires_retry(uint64_t now, uint64_t rto) {
    return now > *packet->get_ts() + rto;
  }
  message *get() { return packet; }

  sender_entry(const sender_entry &) = delete;

  sender_entry(sender_entry &&other) {
    packet = other.packet;
    seq = other.seq;
    retransmitted = other.retransmitted;
    other.packet = nullptr;
  }
};

template<auto adaptive_rto = false>
class retransmission_handler {
  static constexpr uint16_t kQueuedPackets = 64;

public:
  struct statistics {
    uint64_t acked, retransmitted, rtt;
    statistics() : acked(0), retransmitted(0) {}
  };
  retransmission_handler(uint64_t rto = rte_get_timer_cycles())
      : unacked_packets(kQueuedPackets), budget(1), seq(min_seq), rtt(),
        rtt_dv(), rto(rto) {}

  uint64_t cleanup_acked_pkts(uint64_t seq, uint64_t now) {
    uint64_t burst_rtt = 0;
    while (!unacked_packets.empty() && unacked_packets.front()->seq < seq) {
      auto *desc = unacked_packets.front();
      if (!desc->retransmitted) {
        auto tsc_d = now - *desc->packet->get_ts();
        if (rtt == 0)
          rtt = tsc_d;
        else
          std::tie(rtt, rtt_dv) = estimate_timeout(rtt, rtt_dv, tsc_d);
        if (burst_rtt == 0)
          burst_rtt = tsc_d;
        else
          burst_rtt = (burst_rtt * 7 + tsc_d) / 8;
        if constexpr(adaptive_rto)
            rto = 8 * (rtt + 4 * rtt_dv); // always include one backoff
        stats.rtt = rtt;
      }
      assert(desc->packet);
      rte_pktmbuf_free(desc->packet);
      unacked_packets.pop_front();
    }
    return burst_rtt;
  }

  bool record_control_pkt(message* msg){
    if (unacked_packets.full() || budget == 0)
      return false;
    msg->inc_refcnt();
    *msg->get_ts() = 0;
    auto *entry = unacked_packets.enqueue(msg, seq, false);
    timeouts.emplace_back(rto, entry, seq++);
    return true;
  }

  bool record_pkt(message *msg,
                  std::invocable<message *, uint64_t> auto &&ctor) {
    if (unacked_packets.full() || budget == 0)
      return false;
    --budget;
    ctor(msg, seq);
    msg->inc_refcnt();
    *msg->get_ts() = 0;
    auto *entry = unacked_packets.enqueue(msg, seq, false);
    timeouts.emplace_back(rto, entry, seq++);
    return true;
  }

  void probe_retransmit(std::invocable<message *> auto &&cb) {
    uint64_t now = rte_get_timer_cycles();
    while (!timeouts.empty()) {
      auto [mrto, entry, seq] = timeouts.front();
      if (seq < least_unacked_pkt) {
        timeouts.pop_front();
        continue;
      }
      auto *msg = entry->packet;
      if (*msg->get_ts() == 0 || !entry->requires_retry(now, mrto))
        break;
      entry->retransmitted = true;
      cb(msg);
      ++stats.retransmitted;
      msg->inc_refcnt();
      *msg->get_ts() = 0;
      timeouts.pop_front();
      timeouts.emplace_back(rto, entry, seq);
    }
  }

  void acknowledge(uint64_t seq, uint16_t budget) {
    if (seq < least_unacked_pkt)
      return;
    stats.acked = seq;
    least_unacked_pkt = seq + 1;
    auto now = rte_get_timer_cycles();
    cleanup_acked_pkts(seq, now);
    update_budget(budget, seq);
  }

  uint64_t get_seq() const { return seq; }
  uint64_t get_srtt() const { return rtt; }

  bool all_acked() const { return least_unacked_pkt == seq; }

  void update_budget(uint16_t budget, uint64_t ack) {
    budget += (budget - (seq - ack));
  }

  const statistics &get_stats() const { return stats; }

private:
  struct timeout {
    uint64_t rto;
    sender_entry *entry;
    uint64_t seq;

    timeout(uint64_t rto, sender_entry *entry, uint64_t seq)
        : rto(rto), entry(entry), seq(seq) {}
  };
  statistics stats;
  indexable_queue<sender_entry> unacked_packets;
  std::deque<timeout> timeouts;
  uint32_t budget;
  uint64_t seq;
  uint64_t least_unacked_pkt = min_seq;
  uint64_t rtt;
  uint64_t rtt_dv;
  uint64_t rto;
};
