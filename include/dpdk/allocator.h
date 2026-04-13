#pragma once
#include <cstdint>
#include <memory>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memory.h>


extern int tsc_dynfield_offset;
inline uint64_t *get_tsc(rte_mbuf *buf) {
  assert(tsc_dynfield_offset != -1);
  return RTE_MBUF_DYNFIELD(buf, tsc_dynfield_offset, uint64_t *);
}

struct dpdk_allocator {
  using backend_data = rte_mbuf_ext_shared_info;
  static constexpr uint16_t kJumboFrameSize = 9001 + sizeof(rte_ether_hdr);

  static void register_ts_field() {
    static const struct rte_mbuf_dynfield tsc_dynfield_desc = {
        .name = "dynfield_tsc",
        .size = sizeof(uint64_t),
        .align = alignof(uint64_t),
        .flags = 0};
    tsc_dynfield_offset = rte_mbuf_dynfield_register(&tsc_dynfield_desc);
  }

  static std::shared_ptr<dpdk_allocator> create(const char *name, unsigned n) {
    auto *pool = rte_pktmbuf_pool_create(
        name, n, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
    assert(pool); 
    return std::make_shared<dpdk_allocator>(pool);
  }

  dpdk_allocator(rte_mempool *pool)
      : pool(pool) {}
  ~dpdk_allocator() {
    rte_mempool_free(pool);
  }
  rte_mempool *get() { return pool; }
  rte_mempool *pool;
};
