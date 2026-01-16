#pragma once

#include "transport/slot.h"
#include "transport/session.h"
#include "session_manager.h"
#include "log.h"
#include "message.h"
#include "util.h"
#include <cstdint>
#include <memory>
#include <rte_ether.h>

class client_iface {
  static constexpr uint16_t kdefaultBurstSize = 32;

public:
  client_iface(uint16_t port, uint16_t txq, uint16_t rxq,
               std::shared_ptr<message_allocator> pool, const con_config &scon_config)
      : scon_config(scon_config), manager(port, txq, rxq, scon_config.ip, pool) {}

  template <bool flush = false>
  bool send_message(client_slot *con, message *message, uint16_t len) {
    FASTT_LOG_DEBUG("Sending new message with len %u\n", len);  
    *message->get_con_ptr() = con;
    con->send_message(message, len);
    if constexpr (flush)
      manager.flush();
    return true;
  }

  message *recv_message(client_slot *con);
  client_session *open_session(const con_config &target, rte_ether_addr &dmac);

  void flush() { manager.flush(); }

private:
  con_config scon_config;
  session_manager<client_session> manager;
};
