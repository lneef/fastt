#pragma once

#include "dev.h"
#include "message.h"
#include "packet_scheduler.h"
#include "queue.h"
#include "util.h"

#include <atomic>
#include <cstdint>

struct fifo : public queue_base<message *, std::atomic> {
  static constexpr uint32_t kdefaultFifoSize = 32;
  fifo() : queue_base(kdefaultFifoSize) {}
  fifo(std::size_t size) : queue_base(size) {}
  fifo *next, *prev;
};

class client_scheduler {
public:
  client_scheduler(netdev *dev) : head(0), tail(0), ps(dev) {
    head.next = &tail;
    head.prev = nullptr;
    tail.next = nullptr;
    tail.prev = &head;
  }

  void add(fifo *con) { intrusive_push_front(head, con); }

  void operator()() {
    static constexpr auto take = [] (packet_scheduler& ps, fifo* f, uint32_t size){
        static constexpr uint32_t kdefaultTakeThres = 4;
        auto to_take = std::min(kdefaultTakeThres, size);
        for(auto i = 0u; i < to_take; ++i){
            ps.add_pkt(*f->front());
            f->pop_front();
        }
    }; 
    while (true) {
      for (auto *it = head.next, *end = &tail; it != end; it = it->next) 
          take(ps, it, it->size());
      ps.flush();
    }
  }

private:
  fifo head, tail;
  packet_scheduler ps;
};
