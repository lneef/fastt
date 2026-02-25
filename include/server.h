#pragma once

#include "connection.h"
#include "message.h"
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
               std::shared_ptr<message_allocator> pool, [[maybe_unused]] uint16_t lcore_id)
      : scon_config(scon_config),
        manager(false, port, txq, rxq, scon_config.ip, pool, this) {}

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

  template<typename F>
      void run(F&& f){
          manager.run(scheduler, std::forward<F>(f));
      }

  statistics get_stats() { return manager.get_stats(); }
private:
  bu::unordered_flat_map<uint16_t, std::function<concurrency::task(concurrency::scheduler&, connection&)>> services;
  concurrency::scheduler scheduler;
  con_config scon_config;
  connection_manager manager;
  friend class connection_manager;
};
