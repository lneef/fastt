#pragma once

#include "message.h"
#include "protocol.h"
#include "util.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <rte_mbuf_core.h>
#include <rte_ring.h>

enum class transaction_state { IN_PROGRESS, DONE };

struct transaction {
  std::atomic<transaction_state> state = transaction_state::DONE;
  struct rx_interface{
    rte_ring *rx_ring;
  } rx_if;
  struct tx_interface{
    rte_ring *tx_ring;
    intrusive_node<tx_interface> node;
    tx_interface(): node(this){}
  } tx_if;
  intrusive_node<transaction> node;

  void finish() { state = transaction_state::DONE; }
  void mark_inprogess() { state = transaction_state::IN_PROGRESS; }

  message *read() {
    message *msg = nullptr;
    rte_ring_sc_dequeue(rx_if.rx_ring, reinterpret_cast<void **>(&msg));
    return msg;
  }

  bool write(message *msg) { return rte_ring_sp_enqueue(tx_if.tx_ring, msg) == 0; }

  transaction() : node(this) {}
};

class transaction_manager {
  static constexpr uint32_t kdefaultSessionCnt = 64;

public:
  transaction_manager(): head(nullptr), tail(nullptr) {
      head.next = &tail;
      tail.prev = &head;
  }

  transaction *handle_input(message *msg) {
    auto *session_hdr = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    auto sid = session_hdr->msg_id;
    msg->shrink_headroom(sizeof(*session_hdr));
    rte_ring_sp_enqueue(streams[sid].rx_if.rx_ring, msg);
    return &streams[sid];
  }

  void move_to_active(transaction* t){
      head.intrusive_push_front(&t->node);
  }

  intrusive_node<transaction>* begin() {return head.next; }
  intrusive_node<transaction>* end() {return &tail;}

private:
  std::array<transaction, kdefaultSessionCnt> streams{};

  intrusive_node<transaction> head, tail;
};
