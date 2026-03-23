#pragma once

#include "connection.h"
#include "dpdk/allocator.h"
#include "slab_allocator.h"
#include "task/task.h"

#include <boost/unordered/unordered_flat_map.hpp>
#include <cstdint>
#include <memory>
#include <utility>

class server_iface {
public:
  server_iface(uint16_t port, uint16_t txq, uint16_t rxq,
               uint32_t sip,
               std::shared_ptr<dpdk_allocator> pool, uint16_t cores)
      : manager(false, port, txq, rxq, sip, pool, this, cores) {}

  void complete() { manager.flush(); };

  template <typename F>
  bool register_service(uint16_t port, F &&service_handler) {
    auto res = services.emplace(port, std::forward<F>(service_handler));
    return res.second;
  }

  concurrency::scheduler& get_scheduler(){
      return scheduler;
  }

  void run() { manager.run(scheduler); }

  slab_allocator *get_alloc() { return manager.get_allocator(); }

  statistics get_stats() { return manager.get_stats(); }

private:
  bu::unordered_flat_map<uint16_t, std::function<concurrency::task(
                                       server_iface &, connection &)>>
      services;
  concurrency::scheduler scheduler;
  connection_manager manager;
  friend class connection_manager;
};
