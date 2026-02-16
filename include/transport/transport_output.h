#pragma once

#include "message.h"
#include "protocol.h"
#include "transport/msg_fragment.h"
#include "util.h"

#include <cstdint>
#include <cstring>
#include <generic/rte_cycles.h>
#include <rte_branch_prediction.h>
#include <rte_mbuf.h>

template <uint32_t width> struct transport_output {
  // reserve some headroom
  static constexpr uint32_t N = 2 * width;
 transport_output(uint64_t min_seq)
      : wnd(N), front(0), mask(N - 1), least_in_window(min_seq), max_rx(0) {}

  uint64_t get_last_acked_packet() const { return least_in_window - 1; }

  bool set(uint64_t seq, message* msg) {
    auto i = index(seq);
    if (beyond_window(seq) || wnd[i])
      return false;
    if (seq > max_rx) {
      max_rx = seq;
      ts = *msg->get_ts();
    }
    wnd[i] = msg;
    return true;
  }

  bool is_set(uint64_t seq) {
    return seq < least_in_window ||
           (seq <= least_in_window + mask && wnd[index(seq)]);
  }

  bool beyond_window(uint64_t seq) { return seq > least_in_window + mask; }

  template <typename F> uint32_t advance(F &&f) {
    assert(mask + 1 == wnd.size());
    uint32_t advanced = 0;
    while (wnd[front]) {
      ++least_in_window;  
      f(wnd[front]);
      wnd[front] = nullptr;
      front = (front + 1) & mask;
      ++advanced;
    }
    return advanced;
  }

  template<typename F> uint32_t reassemble(F &&f){
      unsigned advanced = 0;
      while(wnd[front]){
          ++least_in_window;
          auto *hdr = wnd[front]->data<protocol::ft_header>();
          bool end = hdr->end;
          wnd[front]->shrink_headroom(sizeof(protocol::ft_header));
          message::merge(first, last, wnd[front]);
          wnd[front] = nullptr;
          if(end){
              auto * msg = first;
              first = last = nullptr;
              f(message_buffer(msg, true));
          }
          front = (front + 1) & mask;
          ++advanced;
      }
      return advanced;
  }


  bool inside(uint64_t seq) {
    return seq >= least_in_window && seq <= least_in_window + mask;
  }

  uint32_t capacity(uint32_t min_capacity) const {
    return std::min<uint32_t>(least_in_window + mask - max_rx, min_capacity);
  }

  std::size_t __inline index(std::size_t i) {
    assert(i >= least_in_window);
    return (i - least_in_window + front) & mask;
  }

  bool try_reserve(uint64_t seq) {
    assert(seq >= least_in_window);
    seq -= least_in_window;
    return seq <= mask;
  }

  bool has_holes() { return max_rx != least_in_window - 1; }

  uint16_t copy_bitset(protocol::ft_sack_payload *data) {
    uint16_t id = 0;
    std::memset(data->bit_map, 0,
                (max_rx - least_in_window + 64) /
                    64 * sizeof(uint64_t)); /* 64 since least_in_window is part of the window */
    assert(protocol::ft_sack_payload::kBitMapLen * 64 >= (max_rx - least_in_window));

    for (auto i = least_in_window; i <= max_rx; ++i, ++id) {
      auto ind = get_bit_indices_64(id);
      data->bit_map[ind.first] |= static_cast<uint64_t>(wnd[index(i)] ? 1 : 0)
                                  << ind.second;
    }
    data->bit_map_len = id;

#if 0
    unsigned itval = 0;
    for(auto i = least_in_window, j = least_in_window; i <= max_rx; ++i){
        for(; !wnd[index(j)] && j <= max_rx; ++j)
            ;
        data->itvls[itval++] = {i , j - 1};
        for(; wnd[index(j)] && j <= max_rx; ++j)
            ;
        i = j;
    }
    datat->max_rx = max_rx;
    data->interval_cnt = itval;
#endif
    return id;
  }

  std::size_t last_seq() const { return least_in_window + mask + 1; }

  uint64_t get_ts() {
    auto now = rte_get_timer_cycles() / get_ticks_us();
    return now - ts;
  }

  std::vector<message*> wnd;
  message* first = nullptr, *last = nullptr;
  std::size_t front, mask;
  uint64_t least_in_window;
  uint64_t max_rx;
  uint64_t ts = 0;
};
