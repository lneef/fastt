#pragma once
#include <memory>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memory.h>

struct dpdk_allocator {
  using backend_data = rte_mbuf_ext_shared_info;

  static void populate_backend_data(backend_data *shinfo,
                                    rte_mbuf_extbuf_free_callback_t cb,
                                    void *opaque) {
    shinfo->free_cb = cb;
    shinfo->refcnt = 1;
    shinfo->fcb_opaque = opaque;
  }
  static std::shared_ptr<dpdk_allocator> create(const char *name, unsigned n) {
    auto *pool = rte_pktmbuf_pool_create(
        name, n, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
    assert(pool);

    auto *small = rte_pktmbuf_pool_create((std::string(name) + "small").c_str(),
                                          n, 0, 0, 128, SOCKET_ID_ANY);
    return std::make_shared<dpdk_allocator>(pool, small);
  }
  dpdk_allocator(rte_mempool *pool, rte_mempool *small)
      : pool(pool), small(small) {}
  ~dpdk_allocator() {
    rte_mempool_free(pool);
    rte_mempool_free(small);
  }
  rte_mempool *get() { return pool; }
  rte_mempool *pool;
  rte_mempool *small;
};
