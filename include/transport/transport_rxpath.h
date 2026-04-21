#pragma once

#include "protocol.h"
#include "sgl.h"
#include "slab_allocator.h"
#include "transport/seq.h"

#include <algorithm>
#include <bitset>
#include <cassert>
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

  ack_cb(seq_t seq = {~0u}) : rcv_una(seq), rcv_acked(seq), rcv_high(seq) {}

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
  // reserve some headroom
  static constexpr unsigned kMaxGrantSize = 256;
  static constexpr unsigned kMaxBitMapSize = 2 * kMaxGrantSize;
  transport_rxpath(seq_t max_rx_in_window = {~0u}, seq_t next_seq = {0})
      : max_rx_in_window(max_rx_in_window), next_seq(next_seq) {}

  seq_t get_last_rcvd_in_seq() const { return seq_t{next_seq - 1}; }

  bool is_retransmission(seq_t seq) {
    return seq < next_seq ||
           (seq < next_seq + kMaxBitMapSize && wnd[index(seq)]);
  }

  bool exceeds_capacity(seq_t seq) const {
    return seq >= next_seq + kMaxBitMapSize;
  }

  void insert(seq_t seq, mbuf *pkt, ack_cb &acb) {
    if (seq > acb.rcv_high) {
      acb.rcv_high = seq;
      max_rx_in_window = seq;
    }
    assert(max_rx_in_window - next_seq + 1 <= kMaxBitMapSize);
    wnd.set(index(seq));
    reassemble(seq, mbuf_take_owner_ship(pkt), acb);
    assert(acb.rcv_high == max_rx_in_window);
  }

  void put_dgram(mbuf_ptr &&pkt) {
    auto *hdr = pkt->data<protocol::ft_header>();
    dgram_ts = std::max(pkt->ts, dgram_ts);
    // control frames are freed
    if (hdr->type != protocol::pkt_type::FT_MSG) {
      seen_done = hdr->type == protocol::pkt_type::FT_DONE;
      return;
    }
    pkt->adj(sizeof(protocol::ft_header));
    out.emplace_back(std::move(pkt));
  }

  bool empty() const { return out.empty(); }

  void reassemble(seq_t seq, mbuf_ptr &&pkt, ack_cb &acb) {
    if (seq != next_seq) {
      // adapted from https://github.com/FDio/vpp
      acb.add_dump_ack();
      rb.insert(seq, std::move(pkt));
    } else {
      assert(wnd.test(index(seq)));
      put_dgram(std::move(pkt));
      wnd.reset(index(next_seq));
      ++next_seq;
      while (wnd.test(index(next_seq))) {
        assert(rb.has_elements());
        assert(rb.next_buffered_seq() == next_seq);
        wnd.reset(index(next_seq));
        ++next_seq;
        put_dgram(std::move(rb.front()));
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
    return out.size() > 0;
  }

  ssize_t read(sgl &msgl) {
    if (out.empty()) 
      return -EAGAIN;
    ssize_t rx = 0;
    while (!out.empty()) {
      auto &buffered = out.front();
      msgl.add_segment_safe(std::move(buffered)); 
      ++crds.crds_returned;
      ++rx;
      out.pop_front();
    }
    return rx;
  }

  unsigned get_available_wnd() const { return kMaxGrantSize; }

  bool return_stalled_crds() const {
    return crds.crds_returned >= kMaxGrantSize / 2;
  }

  uint16_t prepare_return_stalled_crds() {
    auto crds_returned = crds.crds_returned;
    crds.crds_returned = 0;
    return crds_returned;
  }

  ~transport_rxpath() = default;

  struct {
    uint16_t crds_returned = 0;
  } crds;
  uint64_t dgram_ts = 0;

  reorder_buffer rb;
  std::deque<mbuf_ptr> out;

  // connection state
  std::bitset<kMaxBitMapSize> wnd;
  seq_t max_rx_in_window;
  seq_t next_seq;
  bool seen_done = false;
};
