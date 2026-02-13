#pragma once

#include "connection.h"
#include "message.h"
#include "transport/transport.h"
#include "util.h"
#include <cstdint>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

enum class slot_state {
  COMPLETED,
  RUNNING,
};

struct slot {
  uint32_t id;
  connection *con;
  message *buffered = nullptr;
  list_hook link;

  void move_to_active(intrusive_list_t<slot> &active) {
    active.push_front(*this);
  }

  void unlink() {
    if (link.is_linked())
      link.unlink();
  }

  void handle_incoming(message *msg) {
    if (buffered) {
      auto *last = rte_pktmbuf_lastseg(buffered);
      last->next = msg;
      buffered->nb_segs += msg->nb_segs;
      buffered->pkt_len += msg->pkt_len;
    } else {
      buffered = msg;
    }
  }

  slot(uint32_t id, connection *con) : id(id), con(con) {}
  slot() = default;

  struct {
    bool send(message *msg) { return slt->con->send_pkt(msg, true, true); }

    bool can_send(){
        return slt->con->capacity() > 0;
    }

    slot *slt;
  } tx_if{this};

  struct {
    message *get() { return slt->buffered; }

    void take() { slt->buffered = nullptr; }

    slot *slt;
  } rx_if{this};
};
