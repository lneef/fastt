#pragma once

#include "connection.h"
#include "message.h"
#include "util.h"
#include <cstdint>
#include <memory>
#include <rte_ether.h>

class transaction_queue;

class client_iface {
  static constexpr uint16_t kdefaultBurstSize = 32;

public:
  client_iface(uint16_t port, uint16_t txq, uint16_t rxq,
               std::shared_ptr<message_allocator> pool,
               const con_config &scon_config)
      : scon_config(scon_config),
        manager(true, port, txq, rxq, scon_config.ip, pool, this) {}

  template <bool flush = true> bool probe_connection_setup_done(connection *con) {
    manager.fetch_from_qpair();  
    if constexpr (flush)
      manager.flush();
    return con->up();
  }

  void poll(){
      manager.poll_client();
  }

  connection *open_connection(const con_config &target, uint16_t rtid, rte_ether_addr &dmac);

  void close(connection* con){
      manager.close(con);
  }

  void flush() { manager.flush(); }

private:
  con_config scon_config;
public:
  connection_manager manager;
};
