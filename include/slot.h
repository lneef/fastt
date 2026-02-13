#pragma once

#include "message.h"
#include "transport/transport.h"
#include "util.h"
#include <cstdint>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

class connection;

struct slot {
  uint32_t id;
  connection *con;
  message *buffered = nullptr;
  list_hook link;

  void unlink() {
    if (link.is_linked())
      link.unlink();
  }

  void move_to_active(intrusive_list_t<slot> &active) {
    active.push_front(*this);
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

  bool send(message *msg);
  bool can_send();

  message *get() { return buffered; }

  void take() { buffered = nullptr; }
};
