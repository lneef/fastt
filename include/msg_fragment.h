#pragma once
#include "debug.h"

#include <bits/types/struct_iovec.h>
#include <cstddef>
#include <cstdint>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memory.h>
#include <rte_mempool.h>

struct msg_hdr {
  void *buf;
  size_t len;
  size_t off = 0;

  void set_data(void *mbuf, size_t mlen){
      buf = mbuf;
      len = mlen;
      off = 0;
  }
};

struct msg_fragment : public rte_mbuf {
  static int timestamp;
  static int init();
  uint64_t *get_ts() { return RTE_MBUF_DYNFIELD(this, timestamp, uint64_t *); }

  void inc_refcnt() {
    auto *buf = static_cast<rte_mbuf *>(this);
    while (buf) {
      rte_pktmbuf_refcnt_update(buf, 1);
      buf = buf->next;
    }
  }

  static inline void merge(msg_fragment *&first, msg_fragment *&last, msg_fragment *seg) {
    if (!first) {
      first = seg;
      last = seg;
    } else {
      last->next = seg;
      first->nb_segs += seg->nb_segs;
      first->pkt_len += seg->pkt_len;
      last = static_cast<msg_fragment *>(rte_pktmbuf_lastseg(seg));
    }
  }

  template <typename T> T *data() { return rte_pktmbuf_mtod(this, T *); }

  template <typename T> T *data(uint16_t off) {
    return rte_pktmbuf_mtod_offset(this, T *, off);
  }
  void *data() { return rte_pktmbuf_mtod(this, void *); }
  uint16_t len() { return data_len; }

  void set_size(uint16_t len) { pkt_len = len; }

  uint16_t ref_cnt() const{ return this->refcnt; }

  template <typename T> T *move_headroom() {
    rte_pktmbuf_prepend(this, sizeof(T));
    return rte_pktmbuf_mtod(this, T *);
  }

  void free() { rte_pktmbuf_free(this); }

  void shrink_headroom(uint16_t len) { rte_pktmbuf_adj(this, len); }
};

static_assert(sizeof(msg_fragment) == sizeof(rte_mbuf), "");

class msg_fragment_allocator {
  static constexpr uint16_t kRequiredHeadRoom = 128;
  static constexpr std::size_t kMempoolCacheSize = 256;
  static constexpr std::size_t kMemBufPrivSize = 0;
  static constexpr std::size_t kMemBufDataRoomSize = RTE_MBUF_DEFAULT_BUF_SIZE;

public:
  msg_fragment_allocator(const char *name, std::size_t elems)
      : pool(rte_pktmbuf_pool_create(name, elems, kMempoolCacheSize,
                                     kMemBufPrivSize, kMemBufDataRoomSize,
                                     SOCKET_ID_ANY)) {
    assert(pool && "allocation failed");
    payload_size = RTE_MBUF_DEFAULT_DATAROOM;
    assert(payload_size > 0);
    FASTT_LOG_DEBUG("Payload Size: %lu\n", payload_size);
  }

  msg_fragment *alloc_msg_fragment(uint16_t data_size) {

    assert(data_size < payload_size);
    if (data_size >= payload_size - kRequiredHeadRoom)
      return nullptr;
    auto *mbuf = rte_pktmbuf_alloc(pool);
    return prepare(mbuf, data_size);
  }

  size_t get_remaining_space() const { return rte_mempool_avail_count(pool); }

  rte_mempool *get() { return pool; }

  static void deallocate(msg_fragment *msg) { rte_pktmbuf_free(msg); }

  ~msg_fragment_allocator() { rte_mempool_free(pool); }

private:
  msg_fragment *prepare(rte_mbuf *mbuf, uint16_t data_size) {
    assert(mbuf);
    if constexpr (RTE_PKTMBUF_HEADROOM < kRequiredHeadRoom)
      rte_pktmbuf_adj(mbuf, kRequiredHeadRoom - RTE_PKTMBUF_HEADROOM);
    auto *msg = static_cast<msg_fragment *>(mbuf);
    msg->data_len = data_size;
    msg->pkt_len = data_size;
    return msg;
  }
  std::size_t payload_size;
  rte_mempool *pool;
};
