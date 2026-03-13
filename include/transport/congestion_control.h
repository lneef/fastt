#pragma once
#include "debug.h"
#include "transport/protocol.h"
#include "util.h"
#include <algorithm>
#include <cstdint>

/*
 * Swift Congestion Control without pacing, so large scale incasts
 * should be avoided (https://dl.acm.org/doi/pdf/10.1145/3387514.3406591)
 * it is meant to emphasize application level networking in a unikernel
 * additionly it is only supposed to avoid completely downing the receiver in
 * case of medium scale incasts
 */

static __inline constexpr float fast_inv_sqrt(float val) {
  int32_t i;
  float x2, y;
  x2 = val * 0.5F;
  i = cast::bit_cast<int32_t>(val);
  i = 0x5f3759df - (i >> 1);
  y = cast::bit_cast<float>(i);
  y = y * (1.5f - (x2 * y * y));

  return y;
}

struct swift {
  static constexpr float mss = 1500 - protocol::defs::kftOffset;
  static constexpr float initial_len = 1500 - protocol::defs::kftOffset;
  static constexpr float ai = 16;
  static constexpr float beta = 0.8;
  static constexpr float max_md = 0.5;
  static constexpr uint64_t reset_threshold = 16;
  uint64_t retransmit_cnt, last_decrease;
  float base_target_delay, cwnd_size;
  float pacing = 0;
  const uint64_t min_wd_size;

  swift(uint64_t target_delay)
      :  retransmit_cnt(0), last_decrease(0),
        base_target_delay(target_delay), cwnd_size(initial_len),
        min_wd_size(initial_len) {}

  void on_ack(uint64_t acked, uint64_t now, uint64_t srtt, uint64_t delay) {
    retransmit_cnt = 0;
    bool can_decrease = now - last_decrease > srtt;

    // Skip hop delay
    auto target_delay =
        base_target_delay +
        std::max<float>(
            std::min<float>(fast_inv_sqrt(cwnd_size) * 5.4 - 0.48, 5), 0) * get_ticks_us();
    if (delay < target_delay) {
      cwnd_size += (ai) / cwnd_size * (acked);
    } else if (can_decrease) {
      cwnd_size *= std::max<float>(1 - beta * (delay - target_delay) / delay,
                                   1 - max_md);
      last_decrease = now;
    }
    update_stats();
  }

  void on_retransmission_timeout(std::size_t nb, uint64_t rtt, uint64_t now) {
    if (nb == 0)
      return;
    bool can_decrease = now - last_decrease >= rtt;
    retransmit_cnt += nb;
    if (retransmit_cnt > reset_threshold) {
      cwnd_size = min_wd_size;
    } else if (can_decrease) {
      cwnd_size *= (1 - max_md);
      last_decrease = now;
    }
    update_stats();
  }

  void on_fast_recovery(uint64_t now, uint64_t rtt) {
    retransmit_cnt = 0;
    bool can_decrease = now - last_decrease >= rtt;
    if (can_decrease) {
      cwnd_size = (1 - max_md) * cwnd_size;
      last_decrease = now;
    }
    update_stats();
  }

  void update_stats() {
    // TODO: fix this according to real impl
    // but we currently dont have a pacer
    cwnd_size = std::clamp<float>(cwnd_size, 1 * mss, 128 * mss);
  }

  unsigned space(size_t inflight, size_t requested) const {
    // cwnd could have been decreased by a loss or excess rtt  
    auto cap = std::min<unsigned>(requested, cwnd_size > inflight ? cwnd_size - inflight : 0);
    FASTT_LOG_DEBUG("cwnd%u\n", cap);
    return cap;
  }


};
