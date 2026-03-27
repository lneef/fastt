#pragma once

#include <cstdint>
#include <generic/rte_cycles.h>
#include <memory>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_ring.h>
#include <rte_ring_core.h>
#include <utility>
#include <vector>

#include "dpdk/allocator.h"

struct qp {
  static constexpr unsigned kDefaultRingSize = 1024;
  rte_ring *sq;
  rte_ring *cq;

  qp()
      : sq(static_cast<rte_ring *>(
            rte_malloc(nullptr, rte_ring_get_memsize(kDefaultRingSize),
                       alignof(uint64_t)))),
        cq(static_cast<rte_ring *>(
            rte_malloc(nullptr, rte_ring_get_memsize(kDefaultRingSize),
                       alignof(uint64_t)))) {
    rte_ring_init(sq, nullptr, kDefaultRingSize, RING_F_SP_ENQ | RING_F_SC_DEQ);
    rte_ring_init(cq, nullptr, kDefaultRingSize, RING_F_SP_ENQ | RING_F_SC_DEQ);
  }

  ~qp() {
    rte_ring_free(sq);
    rte_ring_free(cq);
  }

  unsigned sq_free() { return rte_ring_free_count(sq); }

  unsigned sq_size() { return rte_ring_get_size(sq); }

  unsigned cq_free() { return rte_ring_free_count(cq); }

  unsigned cq_size() { return rte_ring_get_size(cq); }

  unsigned sq_put_bulk(rte_mbuf **bufs, unsigned n) {
    return rte_ring_enqueue_bulk(sq, reinterpret_cast<void *const *>(bufs), n,
                                 nullptr);
  }

  unsigned sq_get_bulk(rte_mbuf **bufs, unsigned n) {
    return rte_ring_dequeue_bulk(sq, reinterpret_cast<void **>(bufs), n,
                                 nullptr);
  }

  unsigned cq_put_bulk(rte_mbuf **bufs, unsigned n) {
    return rte_ring_enqueue_bulk(cq, reinterpret_cast<void *const *>(bufs), n,
                                 nullptr);
  }

  unsigned cq_get_bulk(rte_mbuf **bufs, unsigned n) {
    return rte_ring_dequeue_bulk(cq, reinterpret_cast<void **>(bufs), n,
                                 nullptr);
  }
};

struct rx_poll {
  static constexpr unsigned kFreeThres = 256;
  static constexpr unsigned kBurstSize = 32;
  using poll_ctx = std::pair<std::shared_ptr<qp>, uint16_t>;

  std::vector<poll_ctx> rx;
  std::vector<rte_mbuf *> bvec;
  std::vector<rte_mbuf *> fvec;
  unsigned burst;
  uint16_t port;

  rx_poll(uint16_t port, unsigned burst = kBurstSize)
      : bvec(burst), fvec(kFreeThres), burst(burst), port(port) {}

  void register_rx(auto &&...args) { rx.emplace_back(args...); }

  void operator()() {
    for (auto &[qp_rings, qid] : rx) {
      auto cq_free = qp_rings->cq_free();
      if (cq_free < burst) {
        qp_rings->cq_get_bulk(fvec.data(), kFreeThres);
        rte_pktmbuf_free_bulk(fvec.data(), kFreeThres);
      }
      auto blen = rte_eth_rx_burst(port, qid, bvec.data(), burst);
      auto clen = std::min<unsigned>(qp_rings->sq_free(), blen);
      auto now = rte_get_timer_cycles();
      for (auto i = 0u; i < blen; ++i)
        *get_tsc(bvec[i]) = now;
      qp_rings->sq_put_bulk(bvec.data(), clen);
      if (clen < blen)
        rte_pktmbuf_free_bulk(bvec.data() + clen, blen - clen);
    }
  }
};
