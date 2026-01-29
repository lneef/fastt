#pragma once
#include <cstdint>
#include <memory>
#include <rte_lcore.h>
#include <rte_timer.h>


enum class timertype { SINGLE, PERIODICAL };
struct dpdk_timer{

  using timepoint_t = uint64_t;
  using timer_t = rte_timer;
  using timer_cb_t = void(*)(timer_t*, void*);
  dpdk_timer(timertype type) : timer(std::make_unique<rte_timer>()) {
    rte_type = type == timertype::PERIODICAL ? PERIODICAL : SINGLE;
    rte_timer_init(timer.get());
  }

  int reset(timepoint_t tp, timer_cb_t cb, uint16_t lcore_id, void *arg) {
    return rte_timer_reset(timer.get(), tp, rte_type, lcore_id, cb, arg);
  }
  int stop() { return rte_timer_stop(timer.get()); }
  enum rte_timer_type rte_type;
  std::unique_ptr<timer_t> timer;
};

template <typename T> struct timer {
  using timepoint_t = T::timepoint_t;
  using timer_t = T::timer_t;
  using timer_cb_t = T::timer_cb_t;

  T impl;
  timepoint_t timeout;

  template<typename ...Args>
  timer(Args&& ...args): impl(std::forward<Args>(args)...){} 

  int reset(timepoint_t to, timer_cb_t cb, uint16_t thread_id, void *timer_arg) {
    timeout = to;  
    return impl.reset(to, cb, thread_id, timer_arg);
  }

  int stop() { return impl.stop(); }

  timepoint_t get_timout() const {
      return timeout;
  }
};

template<typename T>
struct timer_manager{

}; 

template<>
struct timer_manager<dpdk_timer>{
    int manage(){
        return rte_timer_manage();
    }
};
