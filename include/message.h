#pragma once

#include <cstddef>
#include <cstdint>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>

class connection;

struct message : public rte_mbuf {
  static int timestamp;
  static int con_ptr;
  static int init();
  uint64_t *get_ts() { return RTE_MBUF_DYNFIELD(this, timestamp, uint64_t *); }

  void inc_refcnt(){ return rte_pktmbuf_refcnt_update(this, 1); }

  connection **get_con_ptr() {
    return RTE_MBUF_DYNFIELD(this, timestamp, connection **);
  }

  void *data() { return rte_pktmbuf_mtod(this, void *); }
  uint16_t len() { return data_len; }

  void set_size(uint16_t len){
      pkt_len = len;
  }

  template<typename T>
      T* move_headroom(){
         rte_pktmbuf_prepend(this, sizeof(T));
         return rte_pktmbuf_mtod(this, T*);
      }

  void shrink_headroom(uint16_t len){
      rte_pktmbuf_adj(this, len);
  }
};

static_assert(sizeof(message) == sizeof(rte_mbuf), "");

class message_allocator {
  static constexpr uint16_t kRequiredHeadRoom = 128;
public:
  message_allocator(std::size_t payload_size, rte_mempool* pool)
      : payload_size(payload_size), pool(pool) {
    payload_size = rte_pktmbuf_data_room_size(pool);
  }

  message *alloc_message(uint16_t data_size) {
    if (data_size >= payload_size - kRequiredHeadRoom)
      return nullptr;
    assert(data_size < payload_size);
    auto *mbuf = rte_pktmbuf_alloc(pool);
    return prepare(mbuf, data_size);
  }

  static void deallocate(message *msg){
      rte_pktmbuf_free(msg);
  }

  ~message_allocator(){ rte_mempool_free(pool); }

private:
  message *prepare(rte_mbuf *mbuf, uint16_t data_size) {
    rte_pktmbuf_adj(mbuf, kRequiredHeadRoom);
    auto *msg = static_cast<message *>(mbuf);
    auto **con = msg->get_con_ptr();
    *con = nullptr;
    msg->data_len = data_size;
    return msg;
  }
  std::size_t payload_size;
  rte_mempool* pool;
};
