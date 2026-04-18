#pragma once

#include "arch/ena.h"
#include "arch/nic.h"
#include "qp.h"
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
public:
    static constexpr uint16_t kDefaultInputBurstSize = 256;
    static constexpr uint16_t kDefaultOutputBurstSize = 32;
  qpair(uint16_t port, uint16_t txq, uint16_t rxq, std::shared_ptr<qp>& qp_rings)
      : port(port), txq(txq), rxq(rxq), qp_rings(qp_rings), 
        tx_buffer(static_cast<rte_eth_dev_tx_buffer *>(
            rte_zmalloc(("tx_buffer" + std::to_string(txq)).c_str(),
                        RTE_ETH_TX_BUFFER_SIZE(kDefaultOutputBurstSize),
                        RTE_CACHE_LINE_SIZE))),
        nic_arch(std::make_unique<ena::ena>()) {
    assert(tx_buffer);
    rte_eth_tx_buffer_init(tx_buffer, kDefaultOutputBurstSize);
  };

  ~qpair() {
      rte_pktmbuf_free_bulk(tx_buffer->pkts, tx_buffer->length);
      rte_free(tx_buffer); 
  }

  void enqueue_pkt(rte_mbuf *pkt) {  
    rte_eth_tx_buffer(port, txq, tx_buffer, pkt);
  }

  template<unsigned N> void rx_burst_n(packet_vector<rte_mbuf*, N> &vec){
      auto bs = std::min<unsigned>(kDefaultInputBurstSize, qp_rings->cq_free());
      bs = std::min<unsigned>(bs, qp_rings->sq_size());
      auto rcvd = qp_rings->sq_get_bulk(vec.pkts.data(), bs);
      vec.i = rcvd;
  }

  template<unsigned N> void put_mbufs(packet_vector<rte_mbuf*, N>& vec){
      qp_rings->cq_put_bulk(vec.pkts.data(), vec.i);
  }

  void flush() {
      rte_eth_tx_buffer_flush(port, txq, tx_buffer); 
  }


  uint16_t get_rx_qid() const{
      return rxq;
  }

private:
  uint16_t port;
  uint16_t txq;
  uint16_t rxq;
  std::shared_ptr<qp> qp_rings;
  rte_eth_dev_tx_buffer *tx_buffer;
public:
  std::unique_ptr<nic> nic_arch;
};
