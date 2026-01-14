#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <message.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_lcore.h>

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_ring_core.h>
#include <tuple>
#include <type_traits>

#include "message.h"
#include "packet_if.h"
#include "protocol.h"

#include "retransmission_handler.h"
#include "util.h"

struct receiver_entry {
  uint64_t seq;
  receiver_entry() : seq(0) {}
};

template <typename D> struct ack_observer {
  void process_ack(uint64_t ack_seq, uint64_t now) {
    static_cast<D *>(this)->process_ack_impl(ack_seq, now);
  }
};

struct window {
  static constexpr uint16_t kMaxOustandingPackets = 32;
  window(uint64_t min_seq)
      : front(0), mask(kMaxOustandingPackets - 1), least_in_window(min_seq),
        rtt_est(std::numeric_limits<uint64_t>::max()) {}

  uint64_t get_last_acked_packet() const { return least_in_window - 1; }

  bool set(uint64_t seq, message *msg) {
    auto i = index(seq);
    if (beyond_window(seq) || wd[i])
      return false;
    if (seq > max_acked)
      max_acked = seq;
    wd[i] = true;
    messages[i] = msg;
    return true;
  }

  bool is_set(uint64_t seq) {
    return seq < least_in_window ||
           (seq <= least_in_window + mask && wd[index(seq)]);
  }

  bool has_ready_messages(){ return !output.empty(); }

  bool beyond_window(uint64_t seq) { return seq > least_in_window + mask; }

  uint64_t advance() {
    assert(mask + 1 == wd.size());
    while (wd[front]) {
      if ((least_in_window & mask) == 0) {
        last_round = round;
        round = rte_get_timer_cycles();
        did_resize_in_round = false;
        estimate_rtt();
      }
      wd[front] = false;
      output.push_back(messages[front]);
      front = (front + 1) & mask;
      ++least_in_window;
    }
    return least_in_window - 1;
  }

  uint16_t consume_messages(std::invocable<message *> auto &&f, uint16_t bs){
      uint16_t rcvd = 0;
      while(!output.empty() && rcvd < bs){
          f(output.front());
      }
      return rcvd;
  }

  bool inside(uint64_t seq) {
    return seq >= least_in_window && seq <= least_in_window + mask;
  }

  uint32_t capacity() const { return least_in_window + mask - max_acked; }

  std::size_t __inline index(std::size_t i) {
    assert(i >= least_in_window);
    return (i - least_in_window + front) & mask;
  }

  bool try_reserve(uint64_t seq) {
    assert(seq >= least_in_window);
    seq -= least_in_window;
    return seq <= mask;
  }

  void estimate_rtt() { rtt_est = std::min(rtt_est, round - last_round); }

  uint64_t get_rtt() const { return rtt_est; }

  std::size_t last_seq() const { return least_in_window + mask + 1; }

  std::array<bool, kMaxOustandingPackets> wd{};
  std::array<message *, kMaxOustandingPackets> messages{};
  std::deque<message*> output;
  std::size_t front, mask;
  uint64_t least_in_window;
  uint64_t max_acked = 0;
  uint64_t rtt_est;
  std::size_t acked_in_round = 0;
  uint64_t round = 0, last_round = 0;
  bool did_resize_in_round = true;
};

struct statistics {
  uint64_t retransmitted, acked, sent, ecn;
  double rtt;
  statistics(uint64_t retransmitted, uint64_t acked, uint64_t sent,
             uint64_t ecn, uint64_t rtt_est)
      : retransmitted(retransmitted), acked(acked), sent(sent), ecn(ecn) {
    rtt = static_cast<double>(rtt_est) / (rte_get_timer_hz() / 1e6);
  }
};

template <typename D> struct seq_observer {
  void process_seq(uint64_t seq) {
    static_cast<D &>(*this).process_seq_impl(seq);
  }
};

struct ack_scheduler : public seq_observer<ack_scheduler> {
  uint64_t last_acked;
  bool pending_from_retry;
  void process_seq_impl(uint64_t seq) { pending_from_retry = seq < last_acked; }

  bool ack_pending(uint64_t seq) {
    return pending_from_retry || seq > last_acked;
  }

  void ack_callback(uint64_t seq) {
    last_acked = seq;
    pending_from_retry = false;
  }

  ack_scheduler() : last_acked(0), pending_from_retry(false) {}
};

template <typename... O>
  requires(std::is_base_of_v<seq_observer<O>, O> && ...)
struct ack_context {
  std::tuple<O *...> observers;

  void process_seq(uint64_t seq) {
    std::apply([seq](auto &&...elems) { (elems->process_seq(seq), ...); },
               observers);
  }
  ack_context(O *&&...observers) : observers((observers)...) {}
};

struct transport {
  window recv_wd;
  retransmission_handler rt_handler;
  ack_scheduler scheduler;
  ack_context<ack_scheduler> ack_ctx;
  message_allocator *allocator;
  packet_if *pkt_if;
  uint16_t sport;
  struct {
    uint64_t sent = 0;
    uint64_t with_ecn = 0;
  } stats;

  transport(message_allocator *allocator, packet_if *pkt_sink, uint16_t sport)
      : recv_wd(min_seq), rt_handler(), scheduler(), ack_ctx(&scheduler),
        allocator(allocator), pkt_if(pkt_sink), sport(sport) {}

  void probe_timeout() {
    rt_handler.probe_retransmit(
        [&](message *msg) { pkt_if->consume_for_retransmission(msg); });
  }

  bool send_pkt(message *pkt, const con_config &target) {
    probe_timeout();
    auto ctor = [&](message *pkt, uint64_t seq) {
      uint64_t ack = 0;
      auto least_in_window = recv_wd.get_last_acked_packet();
      if (scheduler.ack_pending(least_in_window)) {
        ack = least_in_window;
        scheduler.ack_callback(ack);
      }
      protocol::prepare_ft_header(pkt, seq, ack, seq, recv_wd.capacity());
    };

    auto inserted = rt_handler.record_pkt(pkt, ctor);
    if (inserted)
      pkt_if->consume_pkt(pkt, sport, target);
    return inserted;
  }

  statistics get_stats() const {
    auto &rt_stats = rt_handler.get_stats();
    return {rt_stats.retransmitted, rt_stats.acked, stats.sent, stats.with_ecn,
            rt_stats.rtt};
  }

  bool send_acks(const con_config &target) {  
    auto acked = recv_wd.advance();
    if (!scheduler.ack_pending(acked))
      return false;
    auto *msg = protocol::prepare_ack_pkt(acked, allocator, recv_wd.capacity());
    pkt_if->consume_pkt(msg, sport, target);
    scheduler.ack_callback(acked);
    return true;
  }

  bool process_pkt(message *pkt) {
    auto *hdr = rte_pktmbuf_mtod_offset(pkt, protocol::header_base *,
                                        protocol::defs::kdataOffset);
    switch (hdr->type) {
    case protocol::pkt_type::FT_MSG: {
      auto *nhdr = static_cast<protocol::ft_header *>(hdr);
      if (nhdr->ack)
        rt_handler.acknowledge(nhdr->ack, nhdr->wnd);
      ack_ctx.process_seq(nhdr->seq);
      if (recv_wd.is_set(nhdr->seq)) {   
        rte_pktmbuf_free(pkt);
        return false;
      } else
        recv_wd.set(nhdr->seq, pkt);
      break;
    }
    case protocol::pkt_type::FT_ACK: {
      auto *ahdr = static_cast<protocol::ft_header *>(hdr);
      rt_handler.acknowledge(ahdr->ack, ahdr->wnd);
      rte_pktmbuf_free(pkt);
      break;
    }
    case protocol::pkt_type::FT_INIT: {
      auto *ihdr = static_cast<protocol::init_header*>(hdr) ; 
      if(recv_wd.is_set(ihdr->seq)){
          rte_pktmbuf_free(pkt);                                    
          return false;
      }else
          recv_wd.set(ihdr->seq, pkt);
      break;
    }
    case protocol::pkt_type::FT_INIT_ACK:{
        auto *aihdr = static_cast<protocol::init_ack_header*>(hdr);
        rt_handler.acknowledge(aihdr->seq, aihdr->wnd);
        rte_pktmbuf_free(pkt);
        break;
    }
    default:
      rte_pktmbuf_free(pkt);
      break;
    }
    return true;
  }

  void open_connection(const con_config& target){
      auto *msg = protocol::prepare_init_header(allocator, min_seq);
      bool retval = rt_handler.record_control_pkt(msg); 
      assert(retval);
      pkt_if->consume_pkt(msg, sport, target);
  }

  void accept_connection(const con_config& target){
      recv_wd.advance();
      recv_wd.consume_messages([](message* msg){ rte_pktmbuf_free(msg); }, 1);
      auto *msg = protocol::prepare_init_ack_header(allocator, min_seq, recv_wd.capacity());
      bool retval = rt_handler.record_control_pkt(msg);
      assert(retval);
      pkt_if->consume_pkt(msg, sport, target);
  }

  bool poll(){
      return recv_wd.has_ready_messages();
  }

  uint16_t receive_messages(message **messages, uint16_t bs) {
    probe_timeout();
    return recv_wd.consume_messages(
        [messages, i = 0](message *msg) mutable {
          messages[i++] = msg;
        },
        bs);
  }
};
