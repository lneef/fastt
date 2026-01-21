#pragma once

#include "connection.h"
#include "log.h"
#include "message.h"
#include "util.h"
#include <cstdint>
#include <memory>
#include <rte_ether.h>

struct response_proxy;
class transaction_queue;

class client_iface {
  static constexpr uint16_t kdefaultBurstSize = 32;

public:
  client_iface(uint16_t port, uint16_t txq, uint16_t rxq,
               std::shared_ptr<message_allocator> pool,
               const con_config &scon_config)
      : scon_config(scon_config),
        manager(port, txq, rxq, scon_config.ip, pool) {}

  template <bool flush = false>
  bool send_message(connection *con, message *message, uint16_t len) {
    FASTT_LOG_DEBUG("Sending new message with len %u\n", len);
    *message->get_con_ptr() = con;
    bool sent = con->send_message(message, len);
    if constexpr (flush)
      manager.flush();
    return sent;
  }

  template <bool flush = true> bool probe_connection_setup_done(connection *con) {
    recv_message(con);
    if constexpr (flush)
      manager.flush();
    return con->active();
  }

  message *recv_message(connection *con);
  connection *open_connection(const con_config &target, rte_ether_addr &dmac);

  void flush() { manager.flush(); }

private:
  con_config scon_config;
  connection_manager manager;
};
