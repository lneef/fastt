#pragma once

#include "message.h"
#include "protocol.h"
#include "transport/filter.h"
#include "transport/msg_fragment.h"
#include "util.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <generic/rte_cycles.h>
#include <rte_branch_prediction.h>
#include <rte_mbuf.h>
#include <vector>


struct user_buffer{
    uint8_t *buffer;
    size_t size, off = 0;

    void fill(uint8_t *data, size_t len) {
        auto copy = std::min(len, size - off);
        std::memcpy(buffer + off, data, copy);
        off += copy;
    }
};

template <typename T> struct window_queue {
  std::vector<T *> data;
  size_t head;
  size_t mask;

  T *&operator[](size_t i) {
    assert(i < mask + 1);
    return data[(i + head) & mask];
  }

  T *front() { return data[head]; }

  void pop_front() {
    data[head] = nullptr;
    head = (head + 1) & mask;
  }

  bool new_round() { return head == 0; }

  void advance_head() { head = (head + 1) & mask; }

  size_t capacity() const { return mask + 1; }

  void resize_upon_round() {
    auto nsize = 2 * (mask + 1);
    data.resize(nsize);
    mask = nsize - 1;
  }

  window_queue(std::size_t size)
      : data(size, nullptr), head(0), mask(size - 1) {}
};

struct transport_output {
  // reserve some headroom
  static constexpr uint32_t kRcvSsthresh = 4;
  static constexpr unsigned kLowThreshold = 2048;
  transport_output(uint64_t min_seq, message_allocator *port_allocator, unsigned max_window_size = 128)
      : wnd(kRcvSsthresh), port_allocator(port_allocator), least_in_window(min_seq),
        max_rx(0), max_window_size(max_window_size) {}

  uint64_t get_last_acked_packet() const { return least_in_window - 1; }

  bool set(uint64_t seq, message *msg) {
    auto idx = index(seq);
    if (beyond_window(seq) || wnd[idx])
      return false;
    if (seq > max_rx) {
      max_rx = seq;
      ts = *msg->get_ts();
    }

    // dont buffer in case of imminent buffer exhaustion
    if (unlikely(seq != least_in_window &&
                 port_allocator->get_remaining_space() < kLowThreshold))
      return false;
    wnd[idx] = msg;
    return true;
  }

  bool set_and_reassmble(uint64_t seq, message *msg) {
    if (!set(seq, msg))
      return false;
    reassemble([&](message_buffer&& complete_msg) { out.push_back(complete_msg.buffered); });
    return true;
  }

  bool is_set(uint64_t seq) {
    return seq < least_in_window ||
           (seq <= least_in_window + wnd.mask && wnd[index(seq)]);
  }

  bool beyond_window(uint64_t seq) {
    return seq >= least_in_window + wnd.capacity();
  }

  template <typename F> uint32_t advance(F &&f) {
    uint32_t advanced = 0;
    while (wnd.front()) {
      ++least_in_window;
      f(wnd.front());
      // will reach this only if queue wrapped around and we have a new valid
      // packet
      if (wnd.new_round())
        estimate_rcv_rtt();
      wnd.pop_front();
      ++advanced;
    }
    return advanced;
  }

  template <typename F> uint32_t reassemble(F &&f) {
    unsigned advanced = 0;
    while (wnd.front()) {
      ++least_in_window;
      if (wnd.new_round())
        estimate_rcv_rtt();
      auto *mbuf = wnd.front();
      auto *hdr = mbuf->data<protocol::ft_header>();
      bool end = hdr->end;
      mbuf->shrink_headroom(sizeof(protocol::ft_header));
      message::merge(first, last, mbuf);
      if (end) {
        auto *msg = first;
        first = last = nullptr;
        f(message_buffer(msg, true));
      }
      wnd.pop_front();
      ++advanced;
    }
    return advanced;
  }

  bool inside(uint64_t seq) {
    return seq >= least_in_window && seq < least_in_window + wnd.capacity();
  }

  uint32_t capacity() const {
    return std::min<uint32_t>(least_in_window + wnd.mask - max_rx,
                              max_window_size);
  }

  std::size_t __inline index(std::size_t i) {
    assert(i >= least_in_window);
    return (i - least_in_window);
  }

  bool has_holes() { return max_rx != least_in_window - 1; }

  uint16_t copy_bitset(protocol::ft_sack_payload *data) {
    uint16_t id = 0;
    std::memset(
        data->bit_map, 0,
        (max_rx - least_in_window + 64) / 64 *
            sizeof(
                uint64_t)); /* 64 since least_in_window is part of the window */
    assert(protocol::ft_sack_payload::kBitMapLen * 64 >=
           (max_rx - least_in_window));

    for (auto i = least_in_window; i <= max_rx; ++i, ++id) {
      auto ind = get_bit_indices_64(id);
      data->bit_map[ind.first] |= static_cast<uint64_t>(wnd[index(i)] ? 1 : 0)
                                  << ind.second;
    }
    data->bit_map_len = id;
    return id;
  }

  void estimate_rcv_rtt() {
    round = last_round;
    round = rte_get_timer_cycles();
    did_resize_in_round = false;
    c_rcv_rtt = round - last_round;
    rcv_rtt = filter::min_filter(rcv_rtt, c_rcv_rtt);
  }

  void probe_resize() {
    // we are very conservative here  
    if (c_rcv_rtt <= rcv_rtt && !did_resize_in_round) {
      did_resize_in_round = true;
      wnd.resize_upon_round();
    }
  }

  size_t read(void* buf, size_t size) {
    if (out.empty()) 
        return 0;
    auto *msg = out.front();
    auto to_copy = std::min<size_t>(msg->pkt_len, size);
    assert(to_copy == msg->pkt_len);
    if(msg->nb_segs > 1)
        rte_pktmbuf_read(out.front(), 0, size, buf);
    else
        std::memcpy(buf, msg->data<uint8_t>(), size);
    out.pop_front();
    rte_pktmbuf_free(msg);
    return to_copy;
  }

  uint64_t get_ts() {
    auto now = rte_get_timer_cycles() / get_ticks_us();
    return now - ts;
  }

  window_queue<message> wnd;
  message *first = nullptr, *last = nullptr;
  message_allocator *port_allocator;
  user_buffer buffer{};
  std::deque<message *> out;
  size_t off_read = 0;
  uint64_t least_in_window;
  uint64_t max_rx;
  uint64_t ts = 0;
  uint64_t round = 0, last_round = 0, rcv_rtt = rte_get_timer_hz(), c_rcv_rtt;
  bool did_resize_in_round = false;
  const unsigned max_window_size;
};
