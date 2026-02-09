#pragma once

#include "message.h"
#include "timer.h"
#include "transport/msg_fragment.h"
#include "transport/transport.h"
#include "util.h"
#include "timer.h"
#include <cstdint>
#include <deque>
#include <optional>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_lcore.h>

enum class slot_state {
  COMPLETED,
  RUNNING,
};

struct transaction_slot {
  static constexpr uint32_t kOutStandingMsg = 64;
  std::deque<msg_fragment> incoming;
  list_hook link;
  transport *transport_impl;
  uint64_t incoming_pkts = 0;
  const uint64_t default_timeout;
  timer<dpdk_timer> slot_timer;
  uint16_t tid = 0;
  slot_state state = slot_state::COMPLETED;
  bool is_client = false;
  bool has_outstanding_msgs = false;
  uint8_t sid = 0, tx_sid = 0;

  transaction_slot(uint16_t tid, transport *transport_impl, bool is_client)
      : transport_impl(transport_impl), default_timeout(get_ticks_ms()), slot_timer(timertype::SINGLE),
        tid(tid), is_client(is_client) {
  }

  static void timer_cb(rte_timer *timer, void *arg) {
    (void)timer;
    auto *slot = static_cast<transaction_slot *>(arg);
    slot->transport_impl->acknowledge();
    if (slot->incoming_pkts == 0)
      slot->transport_impl->probe_timeout(slot->tid);
    slot->rearm();
  }

  bool completed() { return state == slot_state::COMPLETED; }

  bool has_outstanding_messages() const {
    return has_outstanding_msgs || incoming.size() > 0;
  }

  bool handle_incoming_server(msg_fragment& msg, uint8_t msid, bool fini) {
    if(msid != sid)
        return false;
    ++sid;
    incoming.push_back(msg);
    ++incoming_pkts;
    has_outstanding_msgs = !fini;
    return true;
  }

  bool handle_incoming_client(msg_fragment& msg, uint8_t msid, bool fini, intrusive_list_t<transaction_slot>& ready) {
    if(msid != sid)
        return false;
    ++sid;
    incoming.push_back(msg);
    ++incoming_pkts;
    if (fini) {
      stop_timer();
      state = slot_state::COMPLETED;
      has_outstanding_msgs = false;
    }
    if(!link.is_linked())
        ready.push_back(*this);
    return true;
  }

  void rearm() {
    incoming_pkts = 0;
    auto timeout = default_timeout *
                   (is_client ? 2 : 1); /* set timeout to 2ms/1ms */
    slot_timer.reset(timeout, timer_cb, rte_lcore_id(), this);

  }

  void stop_timer() {
    incoming_pkts = 0;
    slot_timer.stop();
  }

  void acknowledge() { transport_impl->acknowledge(); }

  void finish() {
    state = slot_state::COMPLETED;
    stop_timer();
    link.unlink();
    assert(!has_outstanding_msgs);
  }

  void update_execution() {
    assert(state == slot_state::COMPLETED);
    state = slot_state::RUNNING;
    has_outstanding_msgs = true;
    rearm();
  }

  bool update_execution_state(intrusive_list_t<transaction_slot> &head) {
    if (state == slot_state::COMPLETED) {
      assert(!link.is_linked());
      head.push_front(*this);
      state = slot_state::RUNNING;
      has_outstanding_msgs = true;
      rearm(); /*rearm timer*/
      return true;
    }
    return false;
  }

  struct {
    std::optional<msg_fragment> read() {  
      if (slot->incoming.empty())
        return std::nullopt;
      auto mf = slot->incoming.front();
      slot->incoming.pop_front();
      return mf;
    }

    bool has_incoming_messages() { return slot->incoming.size() > 0; }

    transaction_slot *slot;
  } rx_if{this};

  struct {
    bool send(message *msg, bool last = false) {
      return slot->transport_impl->send_pkt(msg, slot->tx_sid++, slot->tid, last);
    }

    bool send_streaming(message* msg, bool last = false){
        assert(registered && "Streaming not registered");
        if(!budget)
            return false;
        --budget;
        return slot->transport_impl->send_pkt(msg, slot->tx_sid++, slot->tid, last);
    }

    void alloc_budget(){
        if(!registered){
            slot->transport_impl->register_bulk_stream();
            registered = true;
        }
        budget = slot->transport_impl->alloc_budget();
    }

    void finish_streaming(){
        registered = false;
        slot->transport_impl->deregister_bulk_stream();
    }

    transaction_slot *slot;
    unsigned budget : 31 = 0;
    unsigned registered : 1 = false;
  } tx_if{this};
};
