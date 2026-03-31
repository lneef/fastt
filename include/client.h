#pragma once

#include "connection.h"
#include "dpdk/allocator.h"
#include "util.h"
#include <cstdint>
#include <memory>

class transaction_queue;

class client_iface {
public:
  client_iface(uint16_t port, uint16_t txq, uint16_t rxq,
               std::shared_ptr<dpdk_allocator> pool,
               const con_config &scon_config, uint16_t cores)
      : scon_config(scon_config),
        manager(true, port, txq, rxq, scon_config.ip, pool, this, cores) {}

  connection *open(const con_config &target, rte_ether_addr &dmac) {

    auto *con = open_connection(target, dmac);
    if (!con)
      return nullptr;
    while (!con->up())
      poll();
    return con;
  }

  void close(connection &con) {
    con.close_connection();
    while (!con.all_acked())
      poll();
    manager.close(&con);
  }

  void poll() { manager.poll_client(); }

  void flush() { manager.flush(); }

private:
  connection *open_connection(const con_config &target,                               rte_ether_addr &dmac);
  con_config scon_config;

public:
  connection_manager manager;
};
