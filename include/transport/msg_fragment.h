#pragma once

#include "message.h"
#include <cstdint>
#include <rte_mbuf_core.h>
struct message_buffer {
  message *buffered = nullptr;
  bool done;
  
  message_buffer(message *buffered, bool done = true) : buffered(buffered), done(done) {}
  message_buffer() = default;

  message_buffer(message_buffer &&other) noexcept : buffered(other.buffered), done(other.done) {
    other.buffered = nullptr;
  }

  message_buffer &operator=(message_buffer &&other) noexcept {
    if (this != &other) {
      if (buffered)
        free();
      buffered = other.buffered;
      done = other.done;
      other.buffered = nullptr;
    }
    return *this;
  }

  message& operator->(){
      return *buffered;
  }

  template <typename T> T *data() {
    assert(buffered != nullptr);  
    return rte_pktmbuf_mtod(buffered, T *);
  }

  template<typename T> T* data_offset(uint16_t offset){
      assert(buffered != nullptr);
      return rte_pktmbuf_mtod_offset(buffered, T *, offset);
  }


  void free(){
      rte_pktmbuf_free(buffered);
  }

  void set(message* new_buffered){
      buffered = new_buffered;
  }

  bool ready() const{
      return buffered != nullptr;
  }
};
