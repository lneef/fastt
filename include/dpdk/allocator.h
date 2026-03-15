#pragma once
#include <memory>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memory.h>

struct dpdk_allocator {
  static std::shared_ptr<dpdk_allocator> create(const char *name, unsigned n) {
    auto *pool = rte_pktmbuf_pool_create(
        name, n, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
    assert(pool);
    return std::make_shared<dpdk_allocator>(pool);
  }
  dpdk_allocator(rte_mempool *pool) : pool(pool) {}
  ~dpdk_allocator(){
      rte_mempool_free(pool);
  }
  rte_mempool *get() { return pool; }
  rte_mempool *pool;
};
