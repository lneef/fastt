#pragma once

#include "protocol.h"
#include "sgl.h"
#include "slab_allocator.h"
#include "transport/seq.h"

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <sys/types.h>

struct ack_cb {
  static constexpr size_t kSACKCnt = 3;
  seq_t rcv_una;
  seq_t rcv_acked;
  seq_t rcv_high;
  uint16_t pending_dup_acks = 0;

  void mark_as_acked(seq_t seq) {
    rcv_acked = seq;
    if (pending_dup_acks) {
      if (rcv_acked == rcv_high)
        pending_dup_acks = 0;
      else
        --pending_dup_acks;
    }
    assert(pending_dup_acks == 0 || rcv_high != rcv_acked);
  }

  bool has_unacked_pkts() const {
    return rcv_una > rcv_acked || pending_dup_acks > 0;
  }

  ack_cb(seq_t seq = {~0u}): rcv_una(seq), rcv_acked(seq), rcv_high(seq) {}

  void add_dump_ack() {
    // we send at most kSACKCnt
    // adapted from
    // https://github.com/FDio/vpp/blob/4f3b5f9c473aef1afcc2db5c8258e48ba539f988/src/vnet/tcp/tcp_output.c
    // we dont piggy back sacks and can thus send all at once (following swift)
    pending_dup_acks = std::min<uint16_t>(kSACKCnt, pending_dup_acks + 1);
  }
};

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

struct transport_rxpath {
  struct message {
    mbuf_ptr head;
    uint64_t size : 48;
    uint16_t segs : 16;

    message(mbuf *head, uint64_t size, uint16_t segs)
        : head(mbuf_take_owner_ship(head)), size(size), segs(segs) {}
  };
  // reserve some headroom
  static constexpr unsigned kLowThreshold = 128;
  static constexpr unsigned kMaxGrantSize = 128;
  static constexpr unsigned kMaxBitMapSize = 2 * kMaxGrantSize;
  transport_rxpath(seq_t max_rx_in_window = {~0u}, seq_t next_seq = {0}) : max_rx_in_window(max_rx_in_window), next_seq(next_seq) {}

  seq_t get_last_rcvd_in_seq() const { return seq_t{next_seq - 1}; }

  bool is_retransmission(seq_t seq) {
    return seq < next_seq ||
           (seq < next_seq + kMaxBitMapSize && wnd[index(seq)]);
  }

  bool exceeds_capacity(seq_t seq) const {
    return seq >= next_seq + kMaxBitMapSize;
  }

  void insert(seq_t seq, mbuf *pkt, ack_cb &acb) {
    assert(inside(seq));
    assert(!wnd.test(index(seq)));
    if (seq > acb.rcv_high) {
      acb.rcv_high = seq;
      max_rx_in_window = seq;
    }
    wnd.set(index(seq));
    reassemble(seq, mbuf_take_owner_ship(pkt), acb);
    assert(acb.rcv_high == max_rx_in_window);
  }

  void reassemble_single_msg(mbuf *pkt) {
    auto *hdr = pkt->data<protocol::ft_header>();
    // control frames are freed
    if (hdr->type != protocol::pkt_type::FT_MSG) {
      seen_done = hdr->type == protocol::pkt_type::FT_DONE;
      mbuf_free(pkt);
      return;
    }
    reassembly.segs += pkt->nb_segs;
    bool end = hdr->eom;
    pkt->adj(sizeof(protocol::ft_header));
    reassembly.size += pkt->data_len;
    mbuf::merge(reassembly.first, reassembly.last, pkt);
    if (end) {
      out.emplace_back(reassembly.first, reassembly.size, reassembly.segs);
      reassembly.reset();
      reassembly.size = 0;
    }
  }

  bool empty() const { return out.empty() && reassembly.first == nullptr; }

  void reassemble(seq_t seq, mbuf_ptr &&pkt, ack_cb &acb) {
    if (seq != next_seq) {
      // adapted from https://github.com/FDio/vpp
      acb.add_dump_ack();
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
    acb.rcv_una = get_last_rcvd_in_seq();
  }

  bool inside(seq_t seq) {
    return seq >= next_seq && seq < next_seq + kMaxBitMapSize;
  }

  std::size_t __inline index(seq_t i) {
    assert(i >= next_seq);
    return (i.v) & (kMaxBitMapSize - 1);
  }

  bool has_holes() { return max_rx_in_window != next_seq - 1; }

  uint16_t pack_sack(protocol::ft_sack_payload *data) {
    uint16_t id = 0;
    seq_t highest_seq = next_seq + protocol::ft_sack_payload::kBitMapLen * 64;
    if (likely(highest_seq > max_rx_in_window))
      highest_seq = max_rx_in_window;
    std::memset(
        data->bit_map, 0,
        protocol::ft_sack_payload::kBitMapLen *
            sizeof(
                uint64_t)); /* 64 since least_in_window is part of the window */

    for (auto i = next_seq; i <= highest_seq; ++i, ++id) {
      data->bit_map[id / 64] |= static_cast<uint64_t>(wnd[index(i)])
                                  << (id & 63);
    }
    data->bit_map_len = id;
    return id;
  }

  bool has_buffered_mbufs_frags() const {
    return out.size() > 0 || reassembly.first != nullptr;
  }

  ssize_t read(sgl &msgl) {
    if (out.empty())
      return -EAGAIN;
    auto &buffered = out.front();
    msgl.head = std::move(buffered.head);
    msgl.size = buffered.size;
    msgl.segs = buffered.segs;
    out.pop_front();
    return msgl.size;
  }

  unsigned get_available_wnd() const { return kMaxGrantSize; }

  ~transport_rxpath() {
    if (reassembly.first)
      mbuf_free(reassembly.first);
  }

  // pkt reassmbly and buffering
  struct {
    mbuf *first = nullptr, *last = nullptr;
    uint64_t size = 0;
    uint32_t segs = 0;
    void reset() {
      first = last = nullptr;
      segs = 0;
    }
  } reassembly;

  reorder_buffer rb;
  std::deque<message> out;

  // connection state
  std::bitset<kMaxBitMapSize> wnd;
  seq_t max_rx_in_window;
  seq_t next_seq;
  bool seen_done = false;
};
