#pragma once
#include "dpdk/allocator.h"
#include <cstdint>
#include <memory>
#include <rte_ethdev.h>
#include <vector>

namespace fastt {
int init();
};

struct iface {
  using netdev_iface =
      std::tuple<uint16_t, uint16_t, uint16_t>;
  static std::unique_ptr<iface> configure_port(uint16_t port, uint16_t ntx,
                                             uint16_t nrx, std::shared_ptr<dpdk_allocator>& pool, 
                                             const std::vector<uint16_t>& lcore_ids);
  void stop(){
      rte_eth_dev_stop(port);
  }

  uint16_t tx_queues, rx_queues;
  uint16_t port;
  netdev_iface get_slice(uint16_t idx);
};
