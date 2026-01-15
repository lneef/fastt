#pragma once
#include "log.h"

#include <cstddef>
#include <cstdint>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memory.h>
#include <rte_mempool.h>

class connection;

struct message : public rte_mbuf {
  static int timestamp;
  static int con_ptr;
  static int init();
  uint64_t *get_ts() { return RTE_MBUF_DYNFIELD(this, timestamp, uint64_t *); }

  void inc_refcnt() { return rte_pktmbuf_refcnt_update(this, 1); }

  connection **get_con_ptr() {
    return RTE_MBUF_DYNFIELD(this, timestamp, connection **);
  }

  void *data() { return rte_pktmbuf_mtod(this, void *); }
  uint16_t len() { return data_len; }

  void set_size(uint16_t len) { pkt_len = len; }

  template <typename T> T *move_headroom() {
    rte_pktmbuf_prepend(this, sizeof(T));
    return rte_pktmbuf_mtod(this, T *);
  }

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

  static void deallocate(message *msg) { rte_pktmbuf_free(msg); }

  ~message_allocator() { rte_mempool_free(pool); }

private:
  message *prepare(rte_mbuf *mbuf, uint16_t data_size) {
    if constexpr(RTE_PKTMBUF_HEADROOM < kRequiredHeadRoom)  
        rte_pktmbuf_adj(mbuf, kRequiredHeadRoom - RTE_PKTMBUF_HEADROOM);
    auto *msg = static_cast<message *>(mbuf);
    auto **con = msg->get_con_ptr();
    *con = nullptr;
    msg->data_len = data_size;
    return msg;
  }
  std::size_t payload_size;
  rte_mempool *pool;
};
