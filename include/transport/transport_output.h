#pragma once

#include "message.h"
#include "protocol.h"
#include "util.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <generic/rte_cycles.h>
#include <rte_branch_prediction.h>
#include <rte_mbuf.h>
#include <vector>
#include <deque>

struct user_buffer {
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

  window_queue(std::size_t size)
      : data(size, nullptr), head(0), mask(size - 1) {}
};

struct transport_output {
  // reserve some headroom
  static constexpr unsigned kLowThreshold = 2048;
  transport_output(uint64_t min_seq, message_allocator *port_allocator,
                   unsigned max_window_size = 128)
      : wnd(max_window_size * 2), port_allocator(port_allocator),
        received_pkts(min_seq), least_in_window(min_seq), max_rx_in_window(),
        next_seq(min_seq), last_wnd_return(), max_window_size(max_window_size) {
  }

  uint64_t get_last_acked_packet() const { return next_seq - 1; }

  bool set(uint64_t seq, message *msg) {
    auto idx = index(seq);
    if (beyond_window(seq) || wnd[idx])
      return false;
    if (seq > max_rx_in_window) {
      max_rx_in_window = seq;
      ts = *msg->get_ts();
    }

    // dont buffer in case of imminent buffer exhaustion
    if (unlikely(seq != next_seq &&
                 port_allocator->get_remaining_space() < kLowThreshold))
      return false;
    wnd[idx] = msg;
    ++received_pkts;
    reassemble();
    return true;
  }

  bool is_set(uint64_t seq) {
    return seq < next_seq ||
           (seq <= next_seq + wnd.mask && wnd[index(seq)]);
  }

  bool beyond_window(uint64_t seq) {
    return seq >= least_in_window + wnd.capacity();
  }

  template <typename F> bool advance(F &&f) {
      if(out.empty())
          return false;
      auto *msg = out.front();
      out.pop_front();
      f(msg);
      return true;
  }

   void reassemble() {
    while (wnd.front()) {
      ++next_seq;  
      auto *mbuf = wnd.front();
      auto *hdr = mbuf->data<protocol::ft_header>();
      bool end = hdr->end;
      mbuf->shrink_headroom(sizeof(protocol::ft_header));
      message::merge(first, last, mbuf);
      wnd.pop_front();
      if (end) {
        auto *msg = first;
        first = last = nullptr;
        out.push_back(msg);
      }
    }
  }

  bool inside(uint64_t seq) {
    return seq >= least_in_window && seq < least_in_window + wnd.capacity();
  }

  std::size_t __inline index(std::size_t i) {
    assert(i >= next_seq);
    return (i - next_seq);
  }

  bool has_holes() { return max_rx_in_window != next_seq - 1; }

  uint16_t copy_bitset(protocol::ft_sack_payload *data) {
    uint16_t id = 0;
    std::memset(
        data->bit_map, 0,
        (max_rx_in_window - next_seq + 64) / 64 *
            sizeof(
                uint64_t)); /* 64 since least_in_window is part of the window */
    assert(protocol::ft_sack_payload::kBitMapLen * 64 >=
           (max_rx_in_window - next_seq));

    for (auto i = next_seq; i <= max_rx_in_window; ++i, ++id) {
      auto ind = get_bit_indices_64(id);
      data->bit_map[ind.first] |= static_cast<uint64_t>(wnd[index(i)] ? 1 : 0)
                                  << ind.second;
    }
    data->bit_map_len = id;
    return id;
  }

  size_t read(void *buf, size_t size) {
    if(out.empty())
        return 0;
    auto *msg = out.front();
    out.pop_front();
    auto to_copy = std::min<size_t>(msg->pkt_len, size);
    assert(to_copy == msg->pkt_len);
    if (msg->nb_segs > 1)
      rte_pktmbuf_read(msg, 0, to_copy, buf);
    else
      std::memcpy(buf, msg->data<uint8_t>(), size);
    least_in_window += msg->nb_segs;
    rte_pktmbuf_free(msg);
    return to_copy;
  }

  uint64_t get_ts() { return ts; }

  unsigned get_available_wnd() const {
    if (received_pkts < max_rx_in_window)
      return least_in_window + max_window_size - 1 - received_pkts;
    else
      return least_in_window + max_window_size - 1 - max_rx_in_window;
  }

  unsigned prepare_wnd_return() {
    auto wnd = get_available_wnd();
    last_wnd_return = least_in_window;
    return wnd;
  }

  bool check_wnd_return() const {
    return least_in_window - last_wnd_return >= max_window_size >> 1;
  }

  window_queue<message> wnd;
  message *first = nullptr, *last = nullptr;
  message_allocator *port_allocator;
  std::deque<message*> out;
  user_buffer buffer{};

  uint64_t received_pkts;
  uint64_t least_in_window;
  uint64_t max_rx_in_window;
  uint64_t next_seq;
  uint64_t last_wnd_return;

  uint64_t ts = 0;
  bool did_resize_in_round = false;
  const unsigned max_window_size;
};
