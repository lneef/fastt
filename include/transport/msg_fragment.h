#pragma once

#include "message.h"
#include <cstdint>
#include <rte_mbuf_core.h>
struct msg_fragment {
  message *msg = nullptr;
  uint16_t off;
  msg_fragment(message *msg, uint16_t off = 0) : msg(msg), off(off) {}
  msg_fragment() = default;

  template <typename T> T *data() {
    assert(msg != nullptr);  
    return rte_pktmbuf_mtod_offset(msg, T *, off);
  }

  template<typename T> T* data_offset(uint16_t offset){
      assert(msg != nullptr);
      return rte_pktmbuf_mtod_offset(msg, T *, off + offset);
  }

  void move_offset(uint16_t amount){
      off += amount;
  }

  void free(){
      rte_pktmbuf_free(msg);
  }

  void set(message* new_msg){
      msg = new_msg;
  }

  bool ready() const{
      return msg != nullptr;
  }
};
