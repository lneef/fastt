#pragma once
#include "util.h"
#include <algorithm>
#include <cstdint>

/*
 * Swift Congestion Control without pacing, so large scale incasts
 * should be avoided (https://dl.acm.org/doi/pdf/10.1145/3387514.3406591)
 * it is meant to emphasize "user level" networking solution in a unikernel
 */

static __inline constexpr float fast_inv_sqrt(float val) {
  int32_t i;
  float x2, y;
  x2 = val * 0.5F;
  i = std::bit_cast<int32_t>(val);
  i = 0x5f3759df - (i >> 1);
  y = std::bit_cast<float>(i);
  y = y * (1.5f - (x2 * y * y));

  return y;
}

struct swift {
  static constexpr float ai = 32;
  static constexpr float beta = 0.8;
  static constexpr float max_md = 0.5;
  static constexpr uint64_t reset_threshold = 64;
  uint64_t least_in_window, retransmit_cnt, last_decrease;
  float base_target_delay, cwnd_size;
  const uint64_t min_wd_size;

  swift(std::size_t initial_len, uint64_t target_delay)
      : least_in_window(0), retransmit_cnt(0), last_decrease(0),
        base_target_delay(target_delay), cwnd_size(initial_len),
        min_wd_size(std::max<uint64_t>(initial_len >> 8, 1)) {}

  void on_ack(uint64_t ack, uint64_t now, uint64_t srtt, uint64_t delay) {
    retransmit_cnt = 0;
    bool can_decrease = now - last_decrease > srtt * get_ticks_us();

    // Skip hop delay
    auto target_delay =
        base_target_delay +
        std::max<float>(
            std::min<float>(fast_inv_sqrt(cwnd_size) * 5.4 - 0.48, 5), 0);
    if (delay < target_delay) {
      cwnd_size += ai / cwnd_size * (ack - least_in_window);
    } else if (can_decrease) {
      cwnd_size *=
          std::max<float>(1 - beta * (delay - target_delay) / delay, 1 - max_md);
      last_decrease = now;
    }
    least_in_window = ack;
    update_stats();
  }

  bool has_space(uint64_t seq) const {
    return seq < least_in_window + cwnd_size;
  }

  void on_retransmission_timeout(std::size_t nb, uint64_t rtt, uint64_t now) {
    if (nb == 0)
      return;
    bool can_decrease = now - last_decrease >= rtt * get_ticks_us();
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
    bool can_decrease = now - last_decrease >= rtt * get_ticks_us();
    if (can_decrease) {
      cwnd_size = (1 - max_md) * cwnd_size;
      last_decrease = now;
    }
    update_stats();
  }

  void update_stats() {
    // TODO: fix this according to real impl
    // but we currently dont have a pacer
    cwnd_size = std::clamp<float>(cwnd_size, 1, 128);
  }

  unsigned space(uint64_t seq) const {
    return std::max<unsigned>(cwnd_size - (seq - least_in_window - 1), 0);
  }
};
