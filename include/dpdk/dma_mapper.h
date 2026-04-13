#pragma once
#include <cstdint>
#include <rte_dev.h>
#include <rte_ethdev.h>
#include <rte_memory.h>

inline constexpr uint16_t port_id = 0;

inline int dpdk_dma_map(void *addr, uint64_t iova, unsigned n_pages, size_t page_sz,
                 size_t len) {
  int ret;
  ret = rte_extmem_register(addr, len, nullptr, n_pages, page_sz);
  if (ret)
    return ret;
  rte_eth_dev_info dev_info{};
  ret = rte_eth_dev_info_get(port_id, &dev_info);
  if (ret)
    return ret;
  return rte_dev_dma_map(dev_info.device, addr, iova, len);
}

inline int dpdk_dma_unmap(void *addr, uint64_t iova, size_t len) {
  int ret;
  rte_eth_dev_info dev_info{};
  ret = rte_eth_dev_info_get(port_id, &dev_info);
  if (ret)
    return ret;
  ret = rte_dev_dma_unmap(dev_info.device, addr, iova, len);
  if (ret)
    return ret;
  return rte_extmem_unregister(addr, len);
}
