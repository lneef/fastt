#pragma once

#include "message.h"
#include "protocol.h"
#include "transport/transport_input.h"
#include "util.h"

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <rte_branch_prediction.h>
#include <rte_mbuf.h>

struct reorder_buffer {
  using msg_desc_t = std::pair<uint64_t, message *>;
  std::deque<msg_desc_t> msg_desc;

  void insert(uint64_t seq, message *msg) {
    if (msg_desc.empty()) {
      msg_desc.emplace_back(seq, msg);
      return;
    }
    if (seq < msg_desc.front().first) {
      msg_desc.emplace_front(seq, msg);
    } else if (seq > msg_desc.back().first) {
      msg_desc.emplace_back(seq, msg);
    } else {
      msg_desc_t md(seq, msg);
      auto it = std::lower_bound(msg_desc.begin(), msg_desc.end(), md,
                                 [](const auto &elem, const auto &val) {
                                   return elem.first < val.first;
                                 });
      msg_desc.insert(it, {seq, msg});
    }
  }

  uint64_t next_buffered_seq() { return msg_desc.front().first; }

  bool has_elements() const { return msg_desc.size() > 0; }

  message *front() { return msg_desc.front().second; }

  void pop_front() { msg_desc.pop_front(); }
};

struct transport_output {
  // reserve some headroom
  static constexpr unsigned kLowThreshold = 2048;
  static constexpr unsigned kMaxWndSize = 128;
  transport_output(uint64_t min_seq, message_allocator *port_allocator)
      : port_allocator(port_allocator),  max_rx_in_window(), next_seq(min_seq){}

  uint64_t get_last_rcvd_in_seq() const { return next_seq - 1; }

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
    wnd.set(index(seq));
    reassemble(seq, msg);
    return true;
  }

  bool is_set(uint64_t seq) {
    return seq < next_seq || (seq < next_seq + kMaxWndSize && wnd[index(seq)]);
  }

  bool beyond_window(uint64_t seq) {
    return seq >= next_seq + kMaxWndSize;
  }

  template <typename F> bool advance(F &&f) {
    if (out.empty())
      return false;
    auto *msg = out.front();
    out.pop_front();
    grant_to_return += msg->nb_segs;
    f(msg);
    return true;
  }

  void reassemble_single_msg(message *mbuf) {
    auto *hdr = mbuf->data<protocol::ft_header>();
    // control frames are freed
    if(hdr->type != protocol::pkt_type::FT_MSG){
        mbuf->free();
        return;
    }
    bool end = hdr->end;
    mbuf->shrink_headroom(sizeof(protocol::ft_header));
    message::merge(first, last, mbuf);
    if (end) {
      auto *msg = first;
      first = last = nullptr;
      out.push_back(msg);
    }
  }

  void reassemble(uint64_t seq, message *msg) {
    if (seq != next_seq) {
      rb.insert(seq, msg);
    } else {
      assert(wnd.test(index(seq)));
      reassemble_single_msg(msg);
      wnd.reset(index(next_seq));
      ++next_seq;
      while (wnd.test(index(next_seq))) {
        assert(rb.has_elements());
        assert(rb.next_buffered_seq() == next_seq);
        wnd.reset(index(next_seq));
        ++next_seq;
        auto *mbuf = rb.front();
        reassemble_single_msg(mbuf);
        rb.pop_front();
      }
    }
  }

  bool inside(uint64_t seq) {
    return seq >= next_seq && seq < next_seq + kMaxWndSize;
  }

  std::size_t __inline index(std::size_t i) {
    assert(i >= next_seq);
    return (i - min_seq) & (kMaxWndSize - 1);
  }

  bool has_holes() { return max_rx_in_window != next_seq - 1; }

  uint16_t copy_bitset(protocol::ft_sack_payload *data) {
    uint16_t id = 0;
    auto highest_seq = std::min(
        next_seq + protocol::ft_sack_payload::kBitMapLen * 64, max_rx_in_window);
    std::memset(
        data->bit_map, 0,
        (highest_seq - next_seq + 1 + 63) / 64 *
            sizeof(
                uint64_t)); /* 64 since least_in_window is part of the window */
    assert(protocol::ft_sack_payload::kBitMapLen * 64 >=
           (highest_seq - next_seq));

    for (auto i = next_seq; i <= max_rx_in_window; ++i, ++id) {
      auto ind = get_bit_indices_64(id);
      data->bit_map[ind.first] |= static_cast<uint64_t>(wnd[index(i)])
                                  << ind.second;
    }
    data->bit_map_len = id;
    return id;
  }

  ssize_t read_partial_msg(msg_hdr &hdr) {
    if (!first)
      return 0;
    auto to_copy = std::min<size_t>(first->pkt_len - off, hdr.size);
    if (first->nb_segs > 1)
      rte_pktmbuf_read(first, off, to_copy, hdr.buf);
    else
      std::memcpy(hdr.buf, first->data<uint8_t>() + off, to_copy);
    off += to_copy;
    if (off == first->pkt_len) {
      grant_to_return += first->nb_segs;
      first->free();
      first = last = nullptr;
      off = 0;
      hdr.remaining = 0;
    } else {
      hdr.remaining = first->pkt_len - off;
    }
    hdr.flags = 0;
    return to_copy;
  }

  bool has_buffered_messages_frags() const {
    return out.size() > 0 || first != nullptr;
  }

  size_t read(msg_hdr &hdr) {
    hdr.flags = -ENODATA;  
    if (out.empty())
      return read_partial_msg(hdr);
    
    hdr.flags = 0;
    auto *msg = out.front();
    auto to_copy = std::min<size_t>(msg->pkt_len - off, hdr.size);
    if (msg->nb_segs > 1)
      rte_pktmbuf_read(msg, off, to_copy, hdr.buf);
    else
      std::memcpy(hdr.buf, msg->data<uint8_t>() + off, to_copy);
    off += to_copy;

    if (off == msg->pkt_len) {
      grant_to_return += msg->nb_segs;
      out.pop_front();
      msg->free();
      hdr.remaining = 0;
      off = 0;
    } else {
      hdr.remaining = msg->pkt_len - off;
    }
    return to_copy;
  }

  uint64_t get_ts() { return ts; }

  unsigned get_available_wnd() const {
      return grant_to_return;
  }

  unsigned prepare_wnd_return() {
    auto wnd = get_available_wnd();
    grant_to_return = 0;
    return wnd;
  }

  bool check_wnd_return() const {
    return grant_to_return >= kMaxWndSize >> 1;
  }

  // pkt reassmbly and buffering
  message *first = nullptr, *last = nullptr;
  message_allocator *port_allocator;
  reorder_buffer rb;
  std::deque<message *> out;
  size_t off = 0;

  // connection state
  std::bitset<kMaxWndSize> wnd;
  uint64_t max_rx_in_window;
  uint64_t next_seq;
  uint64_t grant_to_return = kMaxWndSize;
  uint64_t ts = 0;
};
