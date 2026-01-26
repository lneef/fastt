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

enum class slot_state {
  COMPLETED,
  RUNNING,
};

struct transaction_slot {
  static constexpr uint32_t kOutStandingMsg = 64;
  std::deque<message *> incoming;
  list_hook link;
  transport *transport_impl;
  uint64_t incoming_pkts = 0;
  std::unique_ptr<rte_timer> timer;
  uint16_t tid = 0;
  slot_state state = slot_state::COMPLETED;
  bool is_client = false;
  bool has_outstanding_msgs = false;

  transaction_slot(uint16_t tid, transport *transport_impl, bool is_client)
      : transport_impl(transport_impl), timer(std::make_unique<rte_timer>()),
        tid(tid), is_client(is_client) {
    rte_timer_init(timer.get());
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

  void handle_incoming_server(message *msg, bool fini) {
    incoming.push_back(msg);
    ++incoming_pkts;
    has_outstanding_msgs = !fini;
  }

  void handle_incoming_client(message *msg, bool fini) {
    incoming.push_back(msg);
    ++incoming_pkts;
    if (fini) {
      transport_impl->acknowledge();
      stop_timer();
      state = slot_state::COMPLETED;
      has_outstanding_msgs = false;
    }
  }

  void rearm() {
    incoming_pkts = 0;
    auto timeout = rte_get_timer_hz() / 1e3 *
                   (is_client ? 2 : 1); /* set timeout to 2ms/1ms */
    rte_timer_reset(timer.get(), timeout, SINGLE, rte_lcore_id(), timer_cb,
                    this);
  }

  void stop_timer() {
    incoming_pkts = 0;
    rte_timer_stop(timer.get());
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
