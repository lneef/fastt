#pragma once

#include "arch/ena.h"
#include "arch/nic.h"
#include "debug.h"
#include "msg_fragment.h"
#include "util.h"
#include <cstdint>
#include <memory>
#include <rte_cycles.h>
#include <rte_ethdev.h>

#include <array>
#include <rte_mbuf.h>

class qpair {
  static constexpr uint16_t kDefaultInputBurstSize = 32;

public:
  qpair(uint16_t port, uint16_t txq, uint16_t rxq)
      : port(port), txq(txq), rxq(rxq), 
        nic_arch(std::make_unique<ena::ena>()) {};

  uint16_t tx_burst(rte_mbuf **pkts, uint16_t cnt) {
    auto now = rte_get_timer_cycles();
    auto sent = rte_eth_tx_burst(port, txq, pkts, cnt);
    for (uint16_t i = 0; i < sent; ++i)
      *static_cast<msg_fragment *>(pkts[i])->get_ts() = now;
    return sent;
  }
 
  template <typename F> void rx_burst(F &&cb) {
    std::array<rte_mbuf *, kDefaultInputBurstSize> pkts;
    auto now = rte_get_timer_cycles();
    auto rcvd =
        rte_eth_rx_burst(port, rxq, pkts.data(), kDefaultInputBurstSize);
    for (uint16_t i = 0; i < rcvd; ++i) {
      *static_cast<msg_fragment *>(pkts[i])->get_ts() = now;
      cb(static_cast<msg_fragment *>(pkts[i]));
    }
  }

  template <unsigned N> void rx_burst(packet_vector<N> &vec) {
    auto now = rte_get_timer_cycles();
    auto rcvd = rte_eth_rx_burst(port, rxq,
                                 reinterpret_cast<rte_mbuf **>(vec.pkts.data()),
                                 vec.pkts.size());
    for (auto i = 0; i < rcvd; ++i)
      *vec.pkts[i]->get_ts() = now;
    vec.i = rcvd;
    ++total_rx;
    no_rx += rcvd == 0;
  }

private:
  uint16_t port;
  uint16_t txq;
  uint16_t rxq;

public:
  std::unique_ptr<nic> nic_arch;
  uint64_t no_rx = 0;
  uint64_t total_rx = 0;
};
