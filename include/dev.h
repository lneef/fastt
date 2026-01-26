#pragma once

#include "debug.h"
#include "message.h"
#include <cstdint>
#include <generic/rte_cycles.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>

#include <array>

class netdev {
  static constexpr uint16_t kDefaultInputBurstSize = 32;
public:
  netdev(uint16_t port, uint16_t txq, uint16_t rxq)
      : to_us(rte_get_timer_hz() / 1e6), port(port), txq(txq), rxq(rxq) {};

  uint16_t tx_burst(rte_mbuf **pkts, uint16_t cnt) {
    auto now = rte_get_timer_cycles() / to_us;   
    auto sent = rte_eth_tx_burst(port, txq, pkts, cnt);
    for(uint16_t i = 0; i < sent; ++i)
        *static_cast<message*>(pkts[i])->get_ts() = now;
    return sent;
  }

  template <typename F> void rx_burst(F &&cb) {
    std::array<rte_mbuf *, kDefaultInputBurstSize> pkts;
    auto now = rte_get_timer_cycles() / to_us;
    auto rcvd =
        rte_eth_rx_burst(port, rxq, pkts.data(), kDefaultInputBurstSize);
    for (uint16_t i = 0; i < rcvd; ++i) {
      *static_cast<message*>(pkts[i])->get_ts() = now;  
      cb(static_cast<message*>(pkts[i]));
    }
  }

private:
  uint64_t to_us = 0;
  uint16_t port;
  uint16_t txq;
  uint16_t rxq;
};
