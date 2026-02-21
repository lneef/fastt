#pragma once
#include "debug.h"

#include <cstddef>
#include <cstdint>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memory.h>
#include <rte_mempool.h>

struct msg_hdr {
  // user provided
  uint8_t *buf;
  ssize_t size;

  union {
    struct {
      bool som, eom;
    };

    struct {
      size_t remaining;
    };
  };

  int flags;
};

struct msg_buf {
  static int timestamp;
  static int init();
  rte_mbuf *mbuf;

  uint64_t *get_ts() { return RTE_MBUF_DYNFIELD(mbuf, timestamp, uint64_t *); }

  void inc_refcnt() {
    auto *buf = mbuf;
    while (buf) {
      rte_pktmbuf_refcnt_update(buf, 1);
      buf = buf->next;
    }
  }

  static inline void merge(msg_buf &first, msg_buf &last, msg_buf seg) {
    if (!first.mbuf) {
      first = seg;
      last = seg;
    } else {
      last.mbuf->next = seg.mbuf;
      first.mbuf->nb_segs += seg.mbuf->nb_segs;
      first.mbuf->pkt_len += seg.mbuf->pkt_len;
      last.mbuf = rte_pktmbuf_lastseg(seg.mbuf);
    }
  }

  template <typename T> T *data() { return rte_pktmbuf_mtod(mbuf, T *); }

  template <typename T> T *data(uint16_t off) {
    return rte_pktmbuf_mtod_offset(mbuf, T *, off);
  }
  void *data() { return rte_pktmbuf_mtod(mbuf, void *); }
  uint16_t len() { return mbuf->data_len; }

  uint32_t pkt_len() const { return mbuf->pkt_len; }


  template <typename T> T *move_headroom() {
    rte_pktmbuf_prepend(mbuf, sizeof(T));
    return rte_pktmbuf_mtod(mbuf, T *);
  }

  void free() { rte_pktmbuf_free(mbuf); }

  void shrink_headroom(uint16_t len) { rte_pktmbuf_adj(mbuf, len); }
};

struct message : public rte_mbuf {
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

  static inline void merge(message *&first, message *&last, message *seg) {
    if (!first) {
      first = seg;
      last = seg;
    } else {
      last->next = seg;
      first->nb_segs += seg->nb_segs;
      first->pkt_len += seg->pkt_len;
      last = static_cast<message *>(rte_pktmbuf_lastseg(seg));
    }
  }

  template <typename T> T *data() { return rte_pktmbuf_mtod(this, T *); }

  template <typename T> T *data(uint16_t off) {
    return rte_pktmbuf_mtod_offset(this, T *, off);
  }
  void *data() { return rte_pktmbuf_mtod(this, void *); }
  uint16_t len() { return data_len; }

  void set_size(uint16_t len) { pkt_len = len; }

  template <typename T> T *move_headroom() {
    rte_pktmbuf_prepend(this, sizeof(T));
    return rte_pktmbuf_mtod(this, T *);
  }

  void free() { rte_pktmbuf_free(this); }

  void shrink_headroom(uint16_t len) { rte_pktmbuf_adj(this, len); }
};

static_assert(sizeof(message) == sizeof(rte_mbuf), "");

class message_allocator {
  static constexpr uint16_t kRequiredHeadRoom = 128;
  static constexpr std::size_t kMempoolCacheSize = 256;
  static constexpr std::size_t kMemBufPrivSize = 0;
  static constexpr std::size_t kMemBufDataRoomSize = RTE_MBUF_DEFAULT_BUF_SIZE;

public:
  message_allocator(const char *name, std::size_t elems)
      : pool(rte_pktmbuf_pool_create(name, elems, kMempoolCacheSize,
                                     kMemBufPrivSize, kMemBufDataRoomSize,
                                     SOCKET_ID_ANY)) {
    assert(pool && "allocation failed");
    payload_size = RTE_MBUF_DEFAULT_DATAROOM;
    assert(payload_size > 0);
    FASTT_LOG_DEBUG("Payload Size: %lu\n", payload_size);
  }

  message *alloc_message(uint16_t data_size) {

    assert(data_size < payload_size);
    if (data_size >= payload_size - kRequiredHeadRoom)
      return nullptr;
    auto *mbuf = rte_pktmbuf_alloc(pool);
    return prepare(mbuf, data_size);
  }

  size_t get_remaining_space() const { return rte_mempool_avail_count(pool); }

  rte_mempool *get() { return pool; }

  static void deallocate(message *msg) { rte_pktmbuf_free(msg); }

  ~message_allocator() { rte_mempool_free(pool); }

private:
  message *prepare(rte_mbuf *mbuf, uint16_t data_size) {
    assert(mbuf);
    if constexpr (RTE_PKTMBUF_HEADROOM < kRequiredHeadRoom)
      rte_pktmbuf_adj(mbuf, kRequiredHeadRoom - RTE_PKTMBUF_HEADROOM);
    auto *msg = static_cast<message *>(mbuf);
    msg->data_len = data_size;
    msg->pkt_len = data_size;
    return msg;
  }
  std::size_t payload_size;
  rte_mempool *pool;
};
