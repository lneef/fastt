#pragma once
#include <memory>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memory.h>

struct dpdk_allocator {
  using backend_data = rte_mbuf_ext_shared_info;
  static constexpr uint16_t kJumboFrameSize = 9001 + sizeof(rte_ether_hdr);

  static std::shared_ptr<dpdk_allocator> create(const char *name, unsigned n) {
    auto *pool = rte_pktmbuf_pool_create(
        name, n, 128, 0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
    assert(pool);

    auto *small = rte_pktmbuf_pool_create((std::string(name) + "small").c_str(),
                                          n, 128, 0, 128, SOCKET_ID_ANY);
    return std::make_shared<dpdk_allocator>(pool, small);
  }

  static rte_mbuf* alloc_jumbo_frame(rte_mempool *pool){
      rte_mbuf* buffers[5];
      rte_mbuf* frame;
      int ret = rte_pktmbuf_alloc_bulk(pool, buffers, 5);
      if(!ret)
          return nullptr;
      buffers[0]->data_len = RTE_MBUF_DEFAULT_DATAROOM;
      frame = buffers[0];
      unsigned i = 1;
      unsigned total = kJumboFrameSize - RTE_MBUF_DEFAULT_DATAROOM;
      for(; i < 4; ++i){
          buffers[i]->data_len = RTE_MBUF_DEFAULT_DATAROOM;
          total -= RTE_MBUF_DEFAULT_DATAROOM;
          rte_pktmbuf_chain(frame, buffers[i]);
      }
      assert(total > 0);
      buffers[i]->data_len = total;
      rte_pktmbuf_chain(frame, buffers[i]);
      return frame;
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
