#pragma once

#include "connection.h"
#include "message.h"
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

  bool send_message(connection *con, message *messages, uint16_t len);
  void flush();

  template <int N> uint16_t poll(poll_state<N> &events) {
    return manager.poll(events);
  }

  connection *accept();

private:
  con_config scon_config;
  connection_manager manager;
};
