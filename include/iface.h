#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mempool.h>
#include <vector>

namespace fastt {
int init();
};

struct iface {
  using netdev_iface =
      std::tuple<uint16_t, uint16_t, uint16_t, std::shared_ptr<rte_mempool>>;
  static std::optional<iface> configure_port(uint16_t port, uint16_t ntx,
                                             uint16_t nrx);
  void stop(){
      rte_eth_dev_stop(port);
  }
  std::vector<std::shared_ptr<rte_mempool>> pools;
  uint16_t tx_queues, rx_queues;
  uint16_t port;
  netdev_iface get_slice(uint16_t idx);
};
