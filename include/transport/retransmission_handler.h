#pragma once
#include <cassert>
#include <cstdint>
#include <message.h>
#include <rte_cycles.h>

#include "debug.h"
#include "filter.h"
#include "message.h"
#include "queue.h"
#include "util.h"

static constexpr uint64_t min_seq = 1;

struct sender_entry {
  list_hook link;
  message *packet;
  uint64_t seq;
  uint16_t tid : 14;
  uint16_t sacked : 1;
  uint16_t retransmitted : 1;
  sender_entry() : packet(nullptr), seq(0), retransmitted(false) {}
  sender_entry(message *packet, uint64_t seq, uint16_t tid, bool retransmitted)
      : packet(packet), seq(seq), tid(tid), sacked(false),
        retransmitted(retransmitted) {}

  bool requires_retry(uint64_t now, uint64_t rto) {
    return now > *packet->get_ts() + rto;
  }
  message *get() { return packet; }

  sender_entry(const sender_entry &) = delete;
};

class retransmission_handler {
  using indexable_queue = queue_base<sender_entry>;
  static constexpr uint64_t kMSecDiv = 1e3;

public:
  struct statistics {
    uint64_t acked, retransmitted, rtt;
    statistics() : acked(0), retransmitted(0) {}
  };
  retransmission_handler(uint32_t queued_packets, uint32_t budget = 1)
      : unacked_packets(2 * queued_packets), budget(budget), seq(min_seq), rtt() {}

  uint64_t cleanup_acked_pkts(uint64_t seq) {
    uint64_t burst_rtt = 0;
    while (!unacked_packets.empty() && unacked_packets.front()->seq <= seq) {
      auto *desc = unacked_packets.front();
      assert(desc->packet);
      rte_pktmbuf_free(desc->packet);
      desc->link.unlink();
      unacked_packets.pop_front();
    }
    return burst_rtt;
  }

  template <typename F> bool record_pkt(uint16_t tid, message *msg, F &&ctor) {
    if (unacked_packets.full() || budget == 0)
      return false;
    --budget;
    ctor(msg, seq);
    msg->inc_refcnt();
    *msg->get_ts() = 0;
    auto *entry = unacked_packets.enqueue(msg, seq++, tid, false);
    send_list.push_front(*entry);
    FASTT_LOG_DEBUG("Enqueue pkt with %lu new budget %u\n", seq - 1, budget);
    return true;
  }

  template <typename F> void probe_retransmit(F &&cb) {
    for (auto &entry : send_list) {
      auto *msg = entry.packet;
      if (*msg->get_ts() == 0)
        break; 
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
    entry->link.unlink();
    send_list.push_front(*entry);
  }

  void acknowledge(uint64_t seq, uint16_t budget, uint64_t now) {
    if (seq < least_unacked_pkt)
      return;
    stats.acked = seq;
    update_srtt(seq, now);
    update_budget(budget, seq);
    cleanup_acked_pkts(seq);
    least_unacked_pkt = seq + 1;
  }

  auto size() { return unacked_packets.size(); }
  void update_srtt(uint64_t seq, uint64_t now) {
    auto &desc = unacked_packets[seq - least_unacked_pkt];
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
  struct timeout {
    uint64_t rto;
    sender_entry *entry;
    uint64_t seq;

    timeout(uint64_t rto, sender_entry *entry, uint64_t seq)
        : rto(rto), entry(entry), seq(seq) {}
  };
  statistics stats;
  indexable_queue unacked_packets;
  intrusive_list_t<sender_entry> send_list;
  uint32_t budget;
  uint64_t seq;
  uint64_t least_unacked_pkt = min_seq;
  uint64_t rtt;
};
