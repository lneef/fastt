#pragma once
#include "debug.h"
#include "transport/protocol.h"
#include "util.h"
#include <algorithm>
#include <cstdint>
#include <generic/rte_cycles.h>
#include <hdr/hdr_histogram.h>

/*
 * Swift Congestion Control without pacing, so large scale incasts
 * should be avoided (https://dl.acm.org/doi/pdf/10.1145/3387514.3406591)
 * it is meant to emphasize application level networking in a unikernel
 * additionly it is only supposed to avoid completely downing the receiver in
 * case of medium scale incasts
 */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * win_minmax.h: windowed min/max tracker by Kathleen Nichols.
 *
 */
namespace linux_tcp{
/* A single data point for our parameterized min-max tracker */
struct minmax_sample {
	uint64_t	t;	/* time measurement was taken */
	uint64_t	v;	/* value measured */
};

/* State for the parameterized min-max tracker */
struct minmax {
	struct minmax_sample s[3];
};

static inline uint64_t minmax_get(const struct minmax *m)
{
	return m->s[0].v;
}

static inline uint64_t minmax_reset(struct minmax *m, uint64_t t, uint64_t meas)
{
	struct minmax_sample val = { .t = t, .v = meas };

	m->s[2] = m->s[1] = m->s[0] = val;
	return m->s[0].v;
}


static uint64_t minmax_subwin_update(struct minmax *m, uint64_t win,
				const struct minmax_sample *val)
{
	uint64_t dt = val->t - m->s[0].t;

	if (unlikely(dt > win)) {
		/*
		 * Passed entire window without a new val so make 2nd
		 * choice the new val & 3rd choice the new 2nd choice.
		 * we may have to iterate this since our 2nd choice
		 * may also be outside the window (we checked on entry
		 * that the third choice was in the window).
		 */
		m->s[0] = m->s[1];
		m->s[1] = m->s[2];
		m->s[2] = *val;
		if (unlikely(val->t - m->s[0].t > win)) {
			m->s[0] = m->s[1];
			m->s[1] = m->s[2];
			m->s[2] = *val;
		}
	} else if (unlikely(m->s[1].t == m->s[0].t) && dt > win/4) {
		/*
		 * We've passed a quarter of the window without a new val
		 * so take a 2nd choice from the 2nd quarter of the window.
		 */
		m->s[2] = m->s[1] = *val;
	} else if (unlikely(m->s[2].t == m->s[1].t) && dt > win/2) {
		/*
		 * We've passed half the window without finding a new val
		 * so take a 3rd choice from the last half of the window
		 */
		m->s[2] = *val;
	}
	return m->s[0].v;
}

/* Check if new measurement updates the 1st, 2nd or 3rd choice max. */
inline uint64_t minmax_running_max(struct minmax *m, uint64_t win, uint64_t t, uint64_t meas)
{
	struct minmax_sample val = { .t = t, .v = meas };

	if (unlikely(val.v >= m->s[0].v) ||	  /* found new max? */
	    unlikely(val.t - m->s[2].t > win))	  /* nothing left in window? */
		return minmax_reset(m, t, meas);  /* forget earlier samples */

	if (unlikely(val.v >= m->s[1].v))
		m->s[2] = m->s[1] = val;
	else if (unlikely(val.v >= m->s[2].v))
		m->s[2] = val;

	return minmax_subwin_update(m, win, &val);
}

/* Check if new measurement updates the 1st, 2nd or 3rd choice min. */
inline uint64_t minmax_running_min(struct minmax *m, uint64_t win, uint64_t t, uint64_t meas)
{
	struct minmax_sample val = { .t = t, .v = meas };

	if (unlikely(val.v <= m->s[0].v) ||	  /* found new min? */
	    unlikely(val.t - m->s[2].t > win))	  /* nothing left in window? */
		return minmax_reset(m, t, meas);  /* forget earlier samples */

	if (unlikely(val.v <= m->s[1].v))
		m->s[2] = m->s[1] = val;
	else if (unlikely(val.v <= m->s[2].v))
		m->s[2] = val;

	return minmax_subwin_update(m, win, &val);
}
}

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
  hdr_histogram *hist;
  linux_tcp::minmax mm{};
  const uint64_t min_wd_size;

  swift(uint64_t target_delay)
      :  retransmit_cnt(0), last_decrease(0),
        base_target_delay(target_delay), cwnd_size(initial_len),
        min_wd_size(initial_len) {
            hdr_init(10, 200, 3, &hist);
            
        }

  void on_ack(uint64_t acked, uint64_t now, uint64_t srtt, uint64_t delay) {
    retransmit_cnt = 0;
    linux_tcp::minmax_running_min(&mm, get_ticks_us() * 100, now, delay);
    bool can_decrease = now - last_decrease > srtt;
    hdr_record_value(hist, delay / get_ticks_us());
    // Skip hop delay
    auto target_delay =
        base_target_delay +
        std::max<float>(
            std::min<float>(fast_inv_sqrt(cwnd_size) * 5.4 - 0.48, 5), 0) * get_ticks_us();
    delay = linux_tcp::minmax_get(&mm);
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
