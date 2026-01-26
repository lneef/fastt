#include "debug.h"
#include "iface.h"
#include "message.h"
#include <cstdint>
#include <memory>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <rte_timer.h>
#include <tuple>

static uint8_t RSS_DEFAULT_KEY[] = {
    0xbe, 0xac, 0x01, 0xfa, 0x6a, 0x42, 0xb7, 0x3b, 0x80, 0x30,
    0xf2, 0x0c, 0x77, 0xcb, 0x2d, 0xa3, 0xae, 0x7b, 0x30, 0xb4,
    0xd0, 0xca, 0x2b, 0xcb, 0x43, 0xa3, 0x8f, 0xb0, 0x41, 0x67,
    0x25, 0x3d, 0x25, 0x5b, 0x0e, 0xc2, 0x6d, 0x5a, 0x56, 0xda};

static constexpr unsigned RSS_KEY_LEN = 40;

int fastt::init() {
  FASTT_LOG_DEBUG("init fasst\n");
  rte_timer_subsystem_init();
  return message::init();
}

static constexpr auto deleter = [](rte_mempool *pool) {
  if (pool)
    rte_mempool_free(pool);
};

static inline int setup_reta(uint16_t port, uint32_t nrx, uint32_t reta_size){
    auto groups = reta_size / RTE_ETH_RETA_GROUP_SIZE;
    std::vector<rte_eth_rss_reta_entry64> reta(groups);

    for(auto i = 0u; i < reta_size; ++i)
        reta[i / RTE_ETH_RETA_GROUP_SIZE].mask = UINT64_MAX;

    for(auto i = 0u; i < reta_size; ++i){
        uint32_t reta_id = i / RTE_ETH_RETA_GROUP_SIZE;
        uint32_t reta_pos = i % RTE_ETH_RETA_GROUP_SIZE;
        uint32_t rss_qid = i % nrx;
        reta[reta_id].reta[reta_pos] = static_cast<uint16_t>(rss_qid);
    }

    int ret = rte_eth_dev_rss_reta_update(port, reta.data(), reta_size);
    if(ret)
        return -1;
    return 0;
}

std::unique_ptr<iface> iface::configure_port(uint16_t port_id, uint16_t ntx,
                                           uint16_t nrx) {
  uint16_t nb_rxd, nb_txd;
  int retval;
  std::unique_ptr<iface> ifc(new iface()); /*c++11*/
  ifc->port = port_id;
  struct rte_eth_dev_info dev_info;
  struct rte_eth_rxconf rxconf{};
  struct rte_eth_txconf txconf{};
  if (!rte_eth_dev_is_valid_port(ifc->port))
    return nullptr;
  rte_eth_conf port_conf{};
  retval = rte_eth_dev_info_get(ifc->port, &dev_info);
  if (retval != 0)
    return nullptr;
  nb_rxd = dev_info.rx_desc_lim.nb_max;
  nb_txd = dev_info.tx_desc_lim.nb_max;

  if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
    port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
  if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM)
    port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
  if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM)
    port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_UDP_CKSUM;

  if (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_UDP_CKSUM)
    port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_UDP_CKSUM;
  if (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM)
    port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_IPV4_CKSUM;
  if (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_RSS_HASH) {
    auto &rssconf = port_conf.rx_adv_conf.rss_conf;
    port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_RSS_HASH;
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    rssconf.algorithm = RTE_ETH_HASH_FUNCTION_DEFAULT;
    rssconf.rss_key = nullptr;
    rssconf.rss_hf =
        RTE_ETH_RSS_NONFRAG_IPV4_UDP & dev_info.flow_type_rss_offloads;
  }
  bool rss = false;
  if(nrx > 0){
    auto& rssconf = port_conf.rx_adv_conf.rss_conf;  
          port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    rssconf.algorithm = RTE_ETH_HASH_FUNCTION_DEFAULT;
    rssconf.rss_key = RSS_DEFAULT_KEY;
    rssconf.rss_key_len = RSS_KEY_LEN;
    rssconf.rss_hf =
        RTE_ETH_RSS_NONFRAG_IPV4_UDP & dev_info.flow_type_rss_offloads;
    rss = true;
  }
  retval = rte_eth_dev_configure(ifc->port, nrx, ntx, &port_conf);
  if (retval != 0)
    return nullptr;

  retval = rte_eth_dev_adjust_nb_rx_tx_desc(ifc->port, &nb_rxd, &nb_txd);
  if (retval)
    return nullptr;
  txconf = dev_info.default_txconf;
  txconf.offloads = port_conf.txmode.offloads;
  rxconf = dev_info.default_rxconf;
  rxconf.offloads = port_conf.rxmode.offloads;
  uint16_t lcore_id = 0;
  uint16_t setup_tx = 0;
  uint16_t setup_rx = 0;
  RTE_LCORE_FOREACH(lcore_id) {
    ifc->pools.emplace_back(
        rte_pktmbuf_pool_create(std::to_string(lcore_id).data(), 2 * nb_rxd,
                                256, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                rte_lcore_to_socket_id(lcore_id)),
        deleter);
    if (rte_eth_rx_queue_setup(ifc->port, setup_rx++, nb_rxd,
                               rte_lcore_to_socket_id(lcore_id), &rxconf,
                               ifc->pools.back().get()))
      return nullptr;
    if (rte_eth_tx_queue_setup(ifc->port, setup_tx++, nb_txd,
                               rte_lcore_to_socket_id(lcore_id), &txconf))
      return nullptr;
  }
  ifc->tx_queues = setup_tx;
  ifc->rx_queues = setup_rx;
  retval = rte_eth_dev_start(ifc->port);
  if (retval < 0)
    return nullptr;
  if(rss)
      retval = setup_reta(ifc->port, nrx, dev_info.reta_size);
  if(retval)
      return nullptr;
  return ifc;
}

iface::netdev_iface iface::get_slice(uint16_t idx) {
  assert(idx < tx_queues);
  return std::make_tuple(port, idx, idx, pools[idx]);
}
