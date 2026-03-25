#pragma once

#include "arch/ena.h"
#include "arch/nic.h"
#include "util.h"
#include <cstdint>
#include <generic/rte_cycles.h>
#include <memory>
#include <rte_build_config.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>

#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_memory.h>
#include <string>

class qpair {
  static constexpr uint16_t kDefaultInputBurstSize = 32;

public:
  qpair(uint16_t port, uint16_t txq, uint16_t rxq)
      : port(port), txq(txq), rxq(rxq),
        tx_buffer(static_cast<rte_eth_dev_tx_buffer *>(
            rte_zmalloc(("tx_buffer" + std::to_string(txq)).c_str(),
                        RTE_ETH_TX_BUFFER_SIZE(kDefaultInputBurstSize),
                        RTE_CACHE_LINE_SIZE))),
        nic_arch(std::make_unique<ena::ena>()) {
    assert(tx_buffer);
    rte_eth_tx_buffer_init(tx_buffer, kDefaultInputBurstSize);
    rte_eth_tx_buffer_set_err_callback(tx_buffer, unsent_cb, this);
  };

  ~qpair() {
      rte_pktmbuf_free_bulk(tx_buffer->pkts, tx_buffer->length);
      rte_free(tx_buffer); 
  }

  void enqueue_pkt(rte_mbuf *pkt) {  
    auto sent = rte_eth_tx_buffer(port, txq, tx_buffer, pkt);
    total_sent += sent;
    if(sent)
        ts_last_flush = rte_get_timer_cycles();

  }

  template <unsigned N> void rx_burst(packet_vector<rte_mbuf*, N> &vec) {
    auto rcvd = rte_eth_rx_burst(port, rxq, vec.pkts.data(), vec.pkts.size());
    vec.i = rcvd;
  }

  void flush() {
      static constexpr uint64_t kFlushThreshold = 5;
      auto now = rte_get_timer_cycles();
      if(now - ts_last_flush < get_ticks_us() * kFlushThreshold)
          return;
      rte_eth_tx_buffer_flush(port, txq, tx_buffer); 
      ts_last_flush = now;
  }

private:
  static void unsent_cb(rte_mbuf** pkts, uint16_t unsent, void* userdata){
      static constexpr uint16_t kRetryTOus = 10;
      auto* qp = static_cast<qpair*>(userdata);
      auto now = rte_get_timer_cycles();
      auto end = now + get_ticks_us() * kRetryTOus;
      auto sent = 0u;
      do{
          sent += rte_eth_tx_burst(qp->port, qp->txq, pkts + sent, unsent - sent);
      }while(sent < unsent && rte_get_timer_cycles() < end);
      if(unsent - sent)
          rte_pktmbuf_free_bulk(pkts + sent, unsent - sent);
      qp->total_sent += sent;
      qp->ts_last_flush = rte_get_timer_cycles();
  }

  uint16_t port;
  uint16_t txq;
  uint16_t rxq;
  rte_eth_dev_tx_buffer *tx_buffer;
  uint64_t total_sent = 0;
  uint64_t ts_last_flush = 0;
public:
  std::unique_ptr<nic> nic_arch;
};
