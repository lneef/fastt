#pragma once

#include "connection.h"
#include "msg_fragment.h"
#include "util.h"

#include <boost/unordered/unordered_flat_map.hpp>
#include <cstdint>
#include <memory>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_mbuf_core.h>
#include <utility>

class server_iface {
public:
  server_iface(uint16_t port, uint16_t txq, uint16_t rxq,
               const con_config &scon_config,
               std::shared_ptr<msg_fragment_allocator> pool, uint16_t cores)
      : scon_config(scon_config),
        manager(false, port, txq, rxq, scon_config.ip, pool, this, cores) {}

  void complete() { manager.flush(); };

  template<typename F>
   void poll(F&& f){
       manager.poll(f);
   }   

  template<typename F>
      bool register_service(uint16_t port, F&& service_handler){
          auto [_, inserted] = services.emplace(port, std::forward<F>(service_handler));
          return inserted;
      }

      void run(){
          manager.run(scheduler);
      }

  statistics get_stats() { return manager.get_stats(); }
private:
  bu::unordered_flat_map<uint16_t, std::function<concurrency::task(concurrency::scheduler&, connection&)>> services;
  concurrency::scheduler scheduler;
  con_config scon_config;
  connection_manager manager;
  friend class connection_manager;
};
