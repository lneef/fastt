#pragma once

#include "protocol.h"
#include "slab_allocator.h"
#include "transport/seq.h"
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
#include <sys/types.h>

struct reorder_buffer {
  using msg_desc_t = std::pair<seq_t, mbuf_ptr>;
  std::deque<msg_desc_t> msg_desc;

  void insert(seq_t seq, mbuf_ptr &&pkt) {
    if (msg_desc.empty()) {
      msg_desc.emplace_back(seq, std::move(pkt));
      return;
    }
    if (seq < msg_desc.front().first) {
      msg_desc.emplace_front(seq, std::move(pkt));
    } else if (seq > msg_desc.back().first) {
      msg_desc.emplace_back(seq, std::move(pkt));
    } else {
      auto it = std::lower_bound(
          msg_desc.begin(), msg_desc.end(), seq,
          [](const auto &elem, const auto &val) { return elem.first < val; });
      msg_desc.insert(it, {seq, std::move(pkt)});
    }
  }

  seq_t next_buffered_seq() const { return msg_desc.front().first; }

  bool has_elements() const { return msg_desc.size() > 0; }

  mbuf_ptr &front() { return msg_desc.front().second; }

  void pop_front() { msg_desc.pop_front(); }
};

struct transport_output {
  struct message {
    mbuf *head;
    uint64_t size : 48;
    uint16_t segs : 16;

    message(mbuf *head, uint64_t size, uint16_t segs)
        : head(head), size(size), segs(segs) {}

    message(message &&o) noexcept = delete; 
    message &operator=(message &&o) noexcept = delete;

    message(const message &) = delete;
    message &operator=(const message &) = delete;

    ~message() {
      if (head)
        mbuf_free(head);
    }
  };
  // reserve some headroom
  static constexpr unsigned kLowThreshold = 128;
  static constexpr unsigned kMaxGrantSize = 128;
  static constexpr unsigned kMaxBitMapSize = 2 * kMaxGrantSize;
  transport_output()
      :  max_rx_in_window(~0), next_seq() {}

  seq_t get_last_rcvd_in_seq() const { return seq_t{next_seq - 1}; }

  bool is_retransmission_or_exceeds_capacity(seq_t seq,
                                             uint64_t &retransmission_cnt) {
    if (is_retransmission(seq)) {
      ++retransmission_cnt;
      return true;
    }
    return exceeds_capacity(seq);
  }

  bool is_retransmission(seq_t seq) {
    return seq < next_seq ||
           (seq < next_seq + kMaxBitMapSize && wnd[index(seq)]);
  }

  bool exceeds_capacity(seq_t seq) { return seq >= next_seq + kMaxBitMapSize; }

  void insert(seq_t seq, mbuf *pkt) {
    assert(inside(seq));
    assert(!wnd.test(index(seq)));
    if (seq > max_rx_in_window)
      max_rx_in_window = seq;
    ++rcvd_pkts;
    wnd.set(index(seq));
    reassemble(seq, mbuf_take_owner_ship(pkt));
  }

  void reassemble_single_msg(mbuf *pkt) {
    auto *hdr = pkt->data<protocol::ft_header>();
    // control frames are freed
    if (hdr->type != protocol::pkt_type::FT_MSG) {
      seen_done = hdr->type == protocol::pkt_type::FT_DONE;
      mbuf_free(pkt);
      return;
    }
    auto som_len = 0u;
    if (hdr->som) {
      auto *msg_hdr =
          pkt->data<protocol::ft_msg_payload>(sizeof(protocol::ft_header));
      reassembly.size = msg_hdr->out;
      som_len = sizeof(protocol::ft_msg_payload);
    }
    reassembly.segs += pkt->nb_segs;
    bool end = hdr->eom;
    pkt->adj(sizeof(protocol::ft_header) + som_len);
    reassembly.rcvd += pkt->data_len;
    mbuf::merge(reassembly.first, reassembly.last, pkt);
    if (end) {
      out.emplace_back(reassembly.first, reassembly.size, reassembly.segs);
      reassembly.reset();
      reassembly.size = 0;
    }
  }

  bool empty() const { return out.empty() && reassembly.first == nullptr; }

  void reassemble(seq_t seq, mbuf_ptr &&pkt) {
    if (seq != next_seq) {
      rb.insert(seq, std::move(pkt));
    } else {
      assert(wnd.test(index(seq)));
      reassemble_single_msg(pkt.release());
      wnd.reset(index(next_seq));
      ++next_seq;
      while (wnd.test(index(next_seq))) {
        assert(rb.has_elements());
        assert(rb.next_buffered_seq() == next_seq);
        wnd.reset(index(next_seq));
        ++next_seq;
        reassemble_single_msg(rb.front().release());
        rb.pop_front();
      }
    }
  }

  bool inside(seq_t seq) {
    return seq >= next_seq && seq < next_seq + kMaxBitMapSize;
  }

  std::size_t __inline index(seq_t i) {
    assert(i >= next_seq);
    return (i.v) & (kMaxBitMapSize - 1);
  }

  bool has_holes() { return max_rx_in_window != next_seq - 1; }

  uint16_t copy_bitset(protocol::ft_sack_payload *data) {
    uint16_t id = 0;
    seq_t highest_seq = next_seq + protocol::ft_sack_payload::kBitMapLen * 64;
    if (likely(highest_seq > max_rx_in_window))
      highest_seq = max_rx_in_window;
    std::memset(
        data->bit_map, 0,
        protocol::ft_sack_payload::kBitMapLen *
            sizeof(
                uint64_t)); /* 64 since least_in_window is part of the window */

    for (auto i = next_seq; i <= max_rx_in_window; ++i, ++id) {
      auto ind = get_bit_indices_64(id);
      data->bit_map[ind.first] |= static_cast<uint64_t>(wnd[index(i)])
                                  << ind.second;
    }
    data->bit_map_len = id;
    return id;
  }

  bool has_buffered_mbufs_frags() const {
    return out.size() > 0 || reassembly.first != nullptr;
  }

  ssize_t read_partial(void *buf, size_t size, size_t &remaining) {
    if (reassembly.first == nullptr)
      return -EAGAIN;
    if (reassembly.size > size) {
      remaining = reassembly.size;
      return -EMSGSIZE;
    }
    auto to_copy = std::min<size_t>(size, reassembly.rcvd);
    reassembly.first->read(buf);
    grant_to_return += reassembly.segs;
    reassembly.size -= to_copy;
    remaining = reassembly.size;
    mbuf_free(reassembly.first);
    reassembly.reset();
    return to_copy;
  }

  ssize_t read(void *buf, size_t size, size_t &remaining) {
    if (out.empty())
      return read_partial(buf, size, remaining);
    auto &buffered = out.front();
    if (buffered.size > size) {
      remaining = buffered.size;
      return -EMSGSIZE;
    }
    auto to_copy = std::min<size_t>(buffered.size, size);
    auto &msg = buffered.head;
    msg->read(buf);
    grant_to_return += buffered.segs;
    out.pop_front();
    remaining = 0;
    return to_copy;
  }

  uint64_t get_ts() const { return ts; }

  unsigned get_available_wnd() const { return grant_to_return; }

  uint16_t prepare_wnd_return() {
    auto wnd = get_available_wnd();
    grant_to_return = 0;
    return wnd;
  }

  bool check_wnd_return() const {
    return grant_to_return >= kMaxGrantSize >> 1;
  }

  uint64_t get_total_rcvd_pkts() const { return rcvd_pkts; }

  ~transport_output() {
    if (reassembly.first)
        mbuf_free(reassembly.first);
  }

  // pkt reassmbly and buffering
  struct {
    mbuf *first = nullptr, *last = nullptr;
    uint64_t size = 0;
    uint32_t rcvd = 0;
    uint32_t segs = 0;

    void reset() {
      first = last = nullptr;
      segs = 0;
      rcvd = 0;
    }
  } reassembly;

  reorder_buffer rb;
  std::deque<message> out;

  // connection state
  std::bitset<kMaxBitMapSize> wnd;
  seq_t max_rx_in_window;
  seq_t next_seq;
  uint64_t grant_to_return = kMaxGrantSize;
  uint64_t rcvd_pkts = 0;
  uint64_t ts = 0;
  bool seen_done = false;
};
