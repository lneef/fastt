#pragma once

#include "message.h"
#include "timer.h"
#include "transport.h"
#include "util.h"
#include <cstdint>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_lcore.h>

enum class slot_state {
  COMPLETED,
  RUNNING,
};

struct transaction_slot {
  static constexpr uint32_t kOutStandingMsg = 64;
  static constexpr uint16_t kmsgBufSize = 8;
  std::array<message *, kmsgBufSize> incoming;
  list_hook link;
  slotted_transport *transport_impl;
  uint64_t incoming_pkts = 0;
  const uint64_t default_timeout;
  timer<dpdk_timer> slot_timer;
  uint16_t tid = 0;
  uint16_t ptr = 0;
  slot_state state = slot_state::COMPLETED;
  bool is_client = false;
  bool has_outstanding_msgs = false;

  transaction_slot(uint16_t tid, slotted_transport *transport_impl,
                   bool is_client)
      : transport_impl(transport_impl), default_timeout(get_ticks_ms()),
        slot_timer(timertype::SINGLE), tid(tid), is_client(is_client) {}

  static void timer_cb(rte_timer *timer, void *arg) {
    (void)timer;
    auto *slot = static_cast<transaction_slot *>(arg);
    slot->transport_impl->acknowledge(slot->tid);
    if (slot->incoming_pkts == 0)
      slot->transport_impl->probe_timeout(slot->tid);
    slot->rearm();
  }

  bool completed() { return state == slot_state::COMPLETED; }

  bool has_outstanding_messages() const {
    return has_outstanding_msgs ||
           transport_impl->has_outstanding_messages(tid) || ptr > 0;
  }

  void handle_incoming_server(bool fini) {
    ++incoming_pkts;
    has_outstanding_msgs = !fini;
  }

  void handle_incoming_client(bool fini) {
    ++incoming_pkts;
    if (fini) {
      stop_timer();
      state = slot_state::COMPLETED;
      has_outstanding_msgs = false;
    }
  }

  void rearm() {
    incoming_pkts = 0;
    auto timeout =
        default_timeout * (is_client ? 2 : 1); /* set timeout to 2ms/1ms */
    slot_timer.reset(timeout, timer_cb, rte_lcore_id(), this);
  }

  void stop_timer() {
    incoming_pkts = 0;
    slot_timer.stop();
  }

  void acknowledge() { transport_impl->acknowledge(tid); }

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
      if (!slot->ptr)
        slot->transport_impl->receive_messages(
            [this](message *msg) {
              if (slot->ptr < kmsgBufSize)
                slot->incoming[slot->ptr++] = msg;
            },
            slot->tid, kmsgBufSize);
      if (!slot->ptr)
        return nullptr;

      return slot->incoming[--slot->ptr];
    }

    bool has_incoming_messages() { return slot->ptr > 0; }

    transaction_slot *slot;
  } rx_if{this};

  struct {
    bool send(message *msg, bool last = false) {
      return slot->transport_impl->send_pkt(msg, slot->tid, last);
    }
    transaction_slot *slot;
  } tx_if{this};
};
