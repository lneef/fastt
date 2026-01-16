#pragma once

#include "transport/slot.h"
#include "transport/session.h"
#include "message.h"
#include "session_manager.h"
#include "util.h"

#include <cstdint>
#include <memory>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_mbuf_core.h>

class server_iface {
public:
  server_iface(uint16_t port, uint16_t txq, uint16_t rxq,
               const con_config &scon_config,
               std::shared_ptr<message_allocator> pool)
      : scon_config(scon_config),
        manager(port, txq, rxq, scon_config.ip, pool) {}

  bool send_message(server_slot *con, message *messages, uint16_t len);
  void flush();

  template <int N> uint16_t poll(poll_state<N, server_slot> &events) {
    return manager.poll(events);
  }

  slot *accept();

private:
  con_config scon_config;
  session_manager<server_session> manager;
};
