#pragma once

#include "message.h"
#include "transport.h"
#include "util.h"
#include <cstdint>
#include <deque>
#include <generic/rte_cycles.h>
#include <memory>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_timer.h>

class transport;

enum class slot_state {
  COMPLETED,
  RUNNING,
};

struct transaction_slot {
  static constexpr uint32_t kOutStandingMsg = 64;
  uint16_t tid = 0;
  slot_state state = slot_state::COMPLETED;
  std::deque<message *> incoming;
  list_hook link;
  transport *transport_impl;
  bool is_client = false;
  uint64_t incoming_pkts = 0;
  std::unique_ptr<rte_timer> timer;

  transaction_slot(uint16_t tid, transport *transport_impl, bool is_client)
      : tid(tid), transport_impl(transport_impl),
        is_client(is_client), timer(std::make_unique<rte_timer>()) {
    rte_timer_init(timer.get());
  }

  static void timer_cb(rte_timer *timer, void *arg) {
    (void)timer;
    auto *slot = static_cast<transaction_slot *>(arg);
    slot->on_timeout();
  }

  bool completed() { return state == slot_state::COMPLETED; }

  void handle_incoming(message *msg, bool fini) {
    incoming.push_back(msg);
    ++incoming_pkts;

    if (fini && is_client) {
      finish();
      transport_impl->acknowledge();
    }
  }

  void rearm() {
    incoming_pkts = 0;
    auto timeout = rte_get_timer_hz() / 1e4 * (is_client ? 2 : 1);
    rte_timer_reset(timer.get(), timeout, SINGLE, rte_lcore_id(), timer_cb,
                    this);
  }

  void on_timeout() {
    transport_impl->acknowledge();
    if (incoming_pkts == 0)
      transport_impl->probe_timeout(tid);
    rearm();
  }

  void acknowledge() { transport_impl->acknowledge(); }

  void finish() {
    state = slot_state::COMPLETED;
    rte_timer_stop(timer.get());
    incoming_pkts = 0;
    link.unlink();
  }

  void update_execution(){
      assert(state == slot_state::COMPLETED);
      state = slot_state::RUNNING;
      rearm();
  }

  bool update_execution_state(intrusive_list_t<transaction_slot>& head){
    if (state == slot_state::COMPLETED) {
      assert(!link.is_linked());  
      head.push_front(*this);  
      state = slot_state::RUNNING;
      rearm(); /*rearm timer*/
      return true;
    }
    return false;
  } 

  struct {
    message *read() {
      if (slot->incoming.empty())
        return nullptr;
      auto *msg = slot->incoming.front();
      slot->incoming.pop_front();
      return msg;
    }

    bool has_incoming_messages() { return slot->incoming.size() > 0; }

    transaction_slot *slot;
  } rx_if{this};

  struct {
    bool send(message *msg, bool last = false) {
      return slot->transport_impl->send_pkt(msg, slot->tid, last);
    }
    transaction_slot *slot;
  } tx_if{this};
};
