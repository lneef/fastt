#pragma once

#include "message.h"
#include "transport/msg_fragment.h"
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
  message_buffer mb;
  list_hook link;

  void unlink() {
    if (link.is_linked())
      link.unlink();
  }

  void move_to_active(intrusive_list_t<slot> &active) {
    assert(!link.is_linked());  
    active.push_front(*this);
  }

  void handle_incoming(message *msg, bool end) {
    if (mb.buffered) {
      auto *last = rte_pktmbuf_lastseg(mb.buffered);
      last->next = msg;
      mb.buffered->nb_segs += msg->nb_segs;
      mb.buffered->pkt_len += msg->pkt_len;
    } else {
      mb.buffered = msg;
    }
    mb.done = end;
  }

  slot(uint32_t id, connection *con) : id(id), con(con) {}
  slot() = default;

  bool send(message *msg);
  bool can_send();

  message_buffer& get() & { return mb; }

  message_buffer&& get() && {return std::move(mb);}

  bool has_message() const{
      return mb.buffered != nullptr;
  }
};
