#include "iface.h"
#include "debug.h"
#include "message.h"
#include <cstdint>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <rte_timer.h>
#include <tuple>

int fastt::init() {
  FASTT_LOG_DEBUG("init fasst\n");
  rte_timer_subsystem_init();
  return message::init();
}

static constexpr auto deleter = [](rte_mempool *pool) {
  if (pool)
    rte_mempool_free(pool);
};

std::optional<iface> iface::configure_port(uint16_t port_id, uint16_t ntx,
                                           uint16_t nrx) {
  uint16_t nb_rxd, nb_txd;
  int retval;
  iface ifc;
  ifc.port = port_id;
  struct rte_eth_dev_info dev_info;
  struct rte_eth_rxconf rxconf{};
  struct rte_eth_txconf txconf{};
  if (!rte_eth_dev_is_valid_port(ifc.port))
    return std::nullopt;
  rte_eth_conf port_conf{};
  retval = rte_eth_dev_info_get(ifc.port, &dev_info);
  if (retval != 0)
    return std::nullopt;
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
  retval = rte_eth_dev_configure(ifc.port, nrx, ntx, &port_conf);
  if (retval != 0)
    return std::nullopt;

  retval = rte_eth_dev_adjust_nb_rx_tx_desc(ifc.port, &nb_rxd, &nb_txd);
  if (retval)
    return std::nullopt;
  txconf = dev_info.default_txconf;
  txconf.offloads = port_conf.txmode.offloads;
  rxconf = dev_info.default_rxconf;
  rxconf.offloads = port_conf.rxmode.offloads;
  uint16_t lcore_id = 0;
  uint16_t setup_tx = 0;
  uint16_t setup_rx = 0;
  RTE_LCORE_FOREACH(lcore_id) {
    ifc.pools.emplace_back(
        rte_pktmbuf_pool_create(std::to_string(lcore_id).data(), 2 * nb_rxd,
                                256, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                rte_lcore_to_socket_id(lcore_id)),
        deleter);
    if (rte_eth_rx_queue_setup(ifc.port, setup_rx++, nb_rxd,
                               rte_lcore_to_socket_id(lcore_id), &rxconf,
                               ifc.pools.back().get()))
      return std::nullopt;
    if (rte_eth_tx_queue_setup(ifc.port, setup_tx++, nb_txd,
                               rte_lcore_to_socket_id(lcore_id), &txconf))
      return std::nullopt;
  }
  ifc.tx_queues = setup_tx;
  ifc.rx_queues = setup_rx;
  retval = rte_eth_dev_start(ifc.port);
  if (retval < 0)
    return std::nullopt;
  return std::optional(ifc);
}

iface::netdev_iface iface::get_slice(uint16_t idx) {
  assert(idx < tx_queues);
  return std::make_tuple(port, idx, idx, pools[idx]);
}
