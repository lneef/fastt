#pragma once

#include "connection.h"
#include "message.h"
#include "util.h"

#include <cstdint>
#include <memory>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_mbuf_core.h>

class server_iface {
public:
  server_iface(uint16_t port, uint16_t txq, uint16_t rxq,
               const con_config &scon_config,
               std::shared_ptr<message_allocator> pool)
      : scon_config(scon_config),
        manager(false, port, txq, rxq, scon_config.ip, pool, rte_lcore_id()) {}

  void complete() { manager.flush(); };

  template<typename F>
   void poll(F&& f){
       manager.poll(f);
   }   

  statistics get_stats() { return manager.get_stats(); }
private:
  con_config scon_config;
  connection_manager manager;
};
