#pragma once

#include "util.h"
#include <bit>
#include <cstdint>

struct timer {
  using cb_t = void (*)(timer &, void *);
  list_hook link;
  cb_t cb;
  void *arg;

  void operator()() { cb(*this, arg); }

  timer(cb_t cb, void *arg) : cb(cb), arg(arg) {}
};

struct timer_node{
    using value_type = timer;
    intrusive_list_t<timer> timers;

    template<typename F>
    void consume(F&& consumer){
        auto sz = timers.size();
        while(sz-- > 0){
            auto& tmr = timers.front();
            timers.pop_front();
            consumer(tmr);
        }
    }

    void push_back(timer &tmr){
        timers.push_back(tmr);
    }
};

template <typename T> class timing_wheel {
public:
  timing_wheel(uint64_t granularity, uint64_t horizon)
      : front_timestamp(rte_get_timer_cycles() / granularity),
        granularity(granularity), slots(std::bit_ceil(horizon / granularity)),
        wheel(slots) {}

  void insert(typename T::value_type &elem, uint64_t ts) {
    ts = (ts) / granularity;
    if (ts <= front_timestamp)
      ts = front_timestamp;
    else if (ts > front_timestamp + (slots - 1))
      ts = front_timestamp + (slots - 1);
    wheel[ts & (slots - 1)].push_back(elem);
  }

  bool beyond_horizon(uint64_t ts) const {
    return ts > (front_timestamp + slots - 1) * granularity;
  }

  template <typename F> void advance(uint64_t now, F &&consumer){
    now /= granularity;
    while (now >= front_timestamp) {
      auto &entry = wheel[now & (slots - 1)];
      entry.consumer(consumer);
    }
  }

private:
  uint64_t front_timestamp, granularity;
  uint32_t slots;
  std::vector<T> wheel;
};


using timer_set = timing_wheel<timer_node>;
