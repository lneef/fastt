#pragma once

#include "message.h"
#include "timer.h"
#include "transport/transport.h"
#include "util.h"
#include <bit>
#include <cstdint>
#include <deque>
#include <rte_branch_prediction.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_lcore.h>

enum class slot_state {
  COMPLETED,
  RUNNING,
};

/*
 * https://github.com/torvalds/linux/blob/master/include/net/tcp.h
 */
static inline bool before(uint16_t sid1, uint16_t sid2) {
  return std::bit_cast<int>(sid1 - sid2) < 0;
}

struct slot_buffer {
  std::deque<message *> msgs;
  std::vector<message *> buffer;
  uint32_t buffered = 0;
  uint16_t ptr = 0;
  uint16_t mask;
  slot_buffer(uint32_t size) : buffer(size), mask(size - 1) {}

  void insert(uint16_t sid, message *msg) {
    if (!before(sid, ptr + mask + 1)) {
      for (; buffer[ptr];) {
        msgs.push_back(buffer[ptr]);
        buffer[ptr] = nullptr;
        ptr = (ptr + 1) & mask;
      }
    }

    buffer[sid & mask] = msg;
    ++buffered;
  }

  message *get() {
    if (unlikely(msgs.size() > 0)) {
      auto *msg = msgs.front();
      msgs.pop_front();
      --buffered;
      return msg;
    } else if (buffer[ptr]) {
      auto *msg = buffer[ptr];
      buffer[ptr] = nullptr;
      ptr = (ptr + 1) & mask;
      --buffered;
      return msg;
    }
    return nullptr;
  }
};

struct transaction_slot {
  slot_buffer pending;
  list_hook link;
  transport *transport_impl;
  uint64_t incoming_pkts = 0;
  const uint64_t default_timeout;
  timer<dpdk_timer> slot_timer;
  uint16_t tid = 0;
  slot_state state = slot_state::COMPLETED;
  bool is_client = false;
  bool has_outstanding_msgs = false;
  uint8_t tx_sid = 0;

  transaction_slot(uint16_t tid, transport *transport_impl, bool is_client)
      : pending(transport::kOustandingMessages), transport_impl(transport_impl),
        default_timeout(get_ticks_ms()), slot_timer(timertype::SINGLE),
        tid(tid), is_client(is_client) {}

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
    return has_outstanding_msgs || pending.buffered > 0;
  }

  bool handle_incoming_server(message *msg, uint8_t msid, bool fini) {
    pending.insert(msid, msg);
    ++incoming_pkts;
    has_outstanding_msgs = !fini;
    return true;
  }

  bool handle_incoming_client(message *msg, uint8_t msid, bool fini,
                              intrusive_list_t<transaction_slot> &ready) {
    pending.insert(msid, msg);
    ++incoming_pkts;
    if (fini) {
      stop_timer();
      state = slot_state::COMPLETED;
      has_outstanding_msgs = false;
    }
    if (!link.is_linked())
      ready.push_back(*this);
    return true;
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
    message *read() { return slot->pending.get(); }

    bool has_incoming_messages() { return slot->pending.buffered > 0; }

    transaction_slot *slot;
  } rx_if{this};

  struct {
    bool send(message *msg, bool last = false) {
      return slot->transport_impl->send_pkt(msg, slot->tx_sid++, slot->tid,
                                            last);
    }

    bool send_streaming(message* msg, bool last = false){
        if(budget == 0)
            return false;
        --budget;
        return slot->transport_impl->send_pkt(msg, slot->tx_sid++, slot->tid, last);
    }

    void alloc_streaming_budget(){

    }

    transaction_slot *slot;
    uint32_t budget = 0;
  } tx_if{this};
};
