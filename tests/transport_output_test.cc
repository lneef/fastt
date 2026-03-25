#include "sgl.h"
#include "slab_allocator.h"

#include "transport/protocol.h"
#include "transport/seq.h"
#include "transport/transport_rxpath.h"

#include <bit>
#include <cstring>
#include <gtest/gtest.h>
#include <ranges>

class TransportOutputTest : public ::testing::Test {
protected:
  void SetUp() override {
    slab = new slab_allocator{};
    to = new transport_rxpath();
  }

  void TearDown() override {
    delete to;
    delete slab;
  }

  mbuf *make_msg(seq_t seq, char payload, [[maybe_unused]] size_t size) {
    return make_frag(seq, true, true, payload);
  }

  mbuf *make_frag(seq_t seq, [[maybe_unused]] bool start, [[maybe_unused]] bool end, char payload = 'A') {
    auto *msg = slab->alloc_default( sizeof(protocol::ft_header) + 1);
    EXPECT_NE(msg, nullptr);
    auto *hdr = msg->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_MSG;
    hdr->seq = seq;
    *msg->data<char>(sizeof(protocol::ft_header)) = payload;
    return msg;
  }

  mbuf *make_ctrl(seq_t seq) {
    auto *msg = slab->alloc_default(sizeof(protocol::ft_header));
    EXPECT_NE(msg, nullptr);
    auto *hdr = msg->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_CRD_UPDATE;
    hdr->seq = seq;
    return msg;
  }

  slab_allocator *slab;
  transport_rxpath *to;
  ack_cb acb{};
};

TEST_F(TransportOutputTest, Reordered) {
    std::vector<seq_t> seqs{{0}, {1}, {3}, {4}, {5}, {6}, {65}};
    std::vector<mbuf*> msgs;
    msgs.reserve(seqs.size());
    for(auto seq : seqs)
        if(seq == seq_t(1))
            msgs.emplace_back(make_ctrl(seq));
        else
            msgs.emplace_back(make_msg(seq, 'A', 1));
    for(auto [i, msg] : std::ranges::enumerate_view(msgs))
        to->insert(seqs[i], msg, acb);
    EXPECT_EQ(to->out.size(), 1);
    protocol::ft_sack_payload py;
    to->pack_sack(&py);
    EXPECT_TRUE(to->has_holes());
    EXPECT_EQ(std::popcount(py.bit_map[0]), 5);
    EXPECT_EQ(std::popcount(py.bit_map[1]), 0);
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(1));
    EXPECT_EQ(py.bit_map_len, 64);
    to->insert({2}, make_msg({2}, 'A', 1), acb);
    EXPECT_EQ(to->out.size(), 6);
    to->pack_sack(&py);
    EXPECT_EQ(py.bit_map[0] & ((1ull << 59) - 1), 1ull << 58);
}

TEST_F(TransportOutputTest, MultiSegmentReassembly) {
    to->insert({0}, make_frag({0}, true, false, 'A'), acb);
    to->insert({1}, make_frag({1}, false, false, 'B'), acb);
    to->insert({2}, make_frag({2}, false, true, 'C'), acb);

    EXPECT_EQ(to->out.size(), 3u);
    to->insert({3}, make_msg({3}, 'A', 1), acb);
    EXPECT_EQ(to->out.size(), 4u);

    sgl msgl;
    auto rd = to->read(msgl);
    EXPECT_EQ(rd, 4);
    EXPECT_EQ(msgl.segs, 4u);
}

TEST_F(TransportOutputTest, ProactiveCreditReturnForBufferedMessage) {

    to->insert({0}, make_frag({0}, true, false, 'A'), acb);
    to->insert({1}, make_frag({1}, false, false, 'B'), acb);
    to->insert({2}, make_frag({2}, false, false, 'C'), acb);
    to->insert({3}, make_frag({3}, false, true, 'D'), acb);

    EXPECT_EQ(to->out.size(), 4u);

    sgl msgl;
    auto rd = to->read(msgl);
    EXPECT_EQ(rd, 4);
    EXPECT_EQ(msgl.segs, 4u);

    // each dgram is a separate segment in the sgl, verify payload order
    char buf[4] = {};
    int i = 0;
    for (auto &seg : msgl) {
        buf[i++] = *seg.data<char>();
    }
    EXPECT_EQ(std::memcmp(buf, "ABCD", 4), 0);
}

TEST_F(TransportOutputTest, InsertBoundary_ExactNextSeq) {
    // seq == next_seq should go directly to reassembly, not reorder buffer
    to->insert({0}, make_msg({0}, 'A', 1), acb);
    EXPECT_EQ(to->out.size(), 1u);
    EXPECT_FALSE(to->rb.has_elements());
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(0));
}

TEST_F(TransportOutputTest, InsertBoundary_OneAboveNextSeq) {
    // seq == next_seq + 1 must go to reorder buffer, not reassembly
    to->insert({1}, make_msg({1}, 'B', 1), acb);
    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_EQ(to->rb.next_buffered_seq(), seq_t(1));
}

TEST_F(TransportOutputTest, InsertBoundary_FillingGapDrainsReorderBuffer) {
    // Insert seq 1 and 2 out of order, then fill the gap with seq 0
    to->insert({1}, make_msg({1}, 'B', 1), acb);
    to->insert({2}, make_msg({2}, 'C', 1), acb);
    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_TRUE(to->rb.has_elements());

    to->insert({0}, make_msg({0}, 'A', 1), acb);
    // All three should drain into out
    EXPECT_EQ(to->out.size(), 3u);
    EXPECT_FALSE(to->rb.has_elements());
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(2));
}

TEST_F(TransportOutputTest, InsertBoundary_GapNotFullyClosed) {
    // Insert 0, 1, 3 — gap at 2 means only 0 and 1 drain
    to->insert({0}, make_msg({0}, 'A', 1), acb);
    to->insert({1}, make_msg({1}, 'B', 1), acb);
    to->insert({3}, make_msg({3}, 'D', 1), acb);
    EXPECT_EQ(to->out.size(), 2u);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(1));

    // Now fill the gap
    to->insert({2}, make_msg({2}, 'C', 1), acb);
    EXPECT_EQ(to->out.size(), 4u);
    EXPECT_FALSE(to->rb.has_elements());
}

TEST_F(TransportOutputTest, InsertBoundary_LastValidSeqInWindow) {
    // seq == next_seq + kMaxBitMapSize - 1 is the last valid index
    constexpr auto last = transport_rxpath::kMaxBitMapSize - 1;
    EXPECT_TRUE(to->inside(seq_t{last}));
    to->insert(seq_t{last}, make_msg(seq_t{last}, 'Z', 1), acb);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_EQ(to->rb.next_buffered_seq(), seq_t(last));
}

TEST_F(TransportOutputTest, InsertBoundary_ExceedsCapacityAtEdge) {
    // seq == next_seq + kMaxBitMapSize is out of window
    constexpr auto oob = transport_rxpath::kMaxBitMapSize;
    EXPECT_FALSE(to->inside(seq_t{oob}));
    EXPECT_TRUE(to->exceeds_capacity(seq_t{oob}));
    // one before is still valid
    EXPECT_TRUE(to->inside(seq_t{oob - 1}));
    EXPECT_FALSE(to->exceeds_capacity(seq_t{oob - 1}));
}

TEST_F(TransportOutputTest, InsertBoundary_RetransmissionDetection) {
    // Insert and advance next_seq by processing seq 0
    to->insert({0}, make_msg({0}, 'A', 1), acb);
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(0));

    // seq 0 is now < next_seq, should be detected as retransmission
    EXPECT_TRUE(to->is_retransmission(seq_t{0}));
    // seq 1 == next_seq, should NOT be a retransmission
    EXPECT_FALSE(to->is_retransmission(seq_t{1}));
}

TEST_F(TransportOutputTest, InsertBoundary_BitsetRetransmissionInWindow) {
    // Insert seq 0 and seq 2 (skip 1)
    to->insert({0}, make_msg({0}, 'A', 1), acb);
    to->insert({2}, make_msg({2}, 'C', 1), acb);
    // next_seq is now 1, seq 2 has its bit set in wnd
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(0));
    // seq 2 is in window and its bit is set => retransmission
    EXPECT_TRUE(to->is_retransmission(seq_t{2}));
    // seq 1 == next_seq, bit not set => not retransmission
    EXPECT_FALSE(to->is_retransmission(seq_t{1}));
    // seq 3 is in window, bit not set => not retransmission
    EXPECT_FALSE(to->is_retransmission(seq_t{3}));
}

TEST_F(TransportOutputTest, InsertBoundary_FullWindow) {
    // Fill the entire 256-slot window out of order, then close the gap at seq 0
    constexpr auto N = transport_rxpath::kMaxBitMapSize; // 256

    // Insert all slots except seq 0 (in forward order, all go to reorder buffer)
    for (uint32_t i = 1; i < N; ++i)
        to->insert(seq_t{i}, make_msg(seq_t{i}, 'A', 1), acb);

    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_EQ(to->rb.next_buffered_seq(), seq_t(1));
    // next_seq is still 0
    EXPECT_EQ(to->get_last_rcvd_in_seq().v + 1, 0u);

    // Boundary: seq N-1 was the last valid slot, seq N would exceed capacity
    EXPECT_FALSE(to->inside(seq_t{N}));
    EXPECT_TRUE(to->exceeds_capacity(seq_t{N}));

    // Now insert seq 0 — should drain the entire reorder buffer
    to->insert(seq_t{0}, make_msg(seq_t{0}, 'A', 1), acb);
    EXPECT_EQ(to->out.size(), N);
    EXPECT_FALSE(to->rb.has_elements());
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(N - 1));
    EXPECT_FALSE(to->has_holes());

    // After draining, window slides: next valid is N, N-1 exceeds nothing
    EXPECT_TRUE(to->inside(seq_t{N}));
    EXPECT_FALSE(to->exceeds_capacity(seq_t{N}));
    EXPECT_TRUE(to->inside(seq_t{N + N - 1}));
    EXPECT_FALSE(to->inside(seq_t{N + N}));
}

TEST_F(TransportOutputTest, InsertBoundary_FullWindowReverseOrder) {
    // Fill 256 slots in reverse order to stress reorder_buffer::insert paths
    constexpr auto N = transport_rxpath::kMaxBitMapSize;

    for (uint32_t i = N - 1; i >= 1; --i)
        to->insert(seq_t{i}, make_msg(seq_t{i}, 'A', 1), acb);

    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_EQ(to->rb.next_buffered_seq(), seq_t(1));

    to->insert(seq_t{0}, make_msg(seq_t{0}, 'A', 1), acb);
    EXPECT_EQ(to->out.size(), N);
    EXPECT_FALSE(to->rb.has_elements());
}

TEST_F(TransportOutputTest, InsertBoundary_FullWindowWithGaps) {
    // Fill even slots 0..254, leaving odd slots as gaps
    constexpr auto N = transport_rxpath::kMaxBitMapSize;

    for (uint32_t i = 0; i < N; i += 2)
        to->insert(seq_t{i}, make_msg(seq_t{i}, 'A', 1), acb);

    // Only seq 0 drained (next_seq advanced to 1, then hit gap at 1)
    EXPECT_EQ(to->out.size(), 1u);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_TRUE(to->has_holes());
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(0));

    // Fill odd slots to close all gaps
    for (uint32_t i = 1; i < N; i += 2)
        to->insert(seq_t{i}, make_msg(seq_t{i}, 'A', 1), acb);

    EXPECT_EQ(to->out.size(), N);
    EXPECT_FALSE(to->rb.has_elements());
    EXPECT_FALSE(to->has_holes());
}

TEST_F(TransportOutputTest, InsertBoundary_WindowSlideAfterFullDrain) {
    // Fill and drain window, then verify the window slides correctly
    constexpr auto N = transport_rxpath::kMaxBitMapSize;

    // Fill in-order to avoid reorder buffer
    for (uint32_t i = 0; i < N; ++i)
        to->insert(seq_t{i}, make_msg(seq_t{i}, 'A', 1), acb);

    EXPECT_EQ(to->out.size(), N);
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(N - 1));

    // Window should now accept [N, 2N)
    EXPECT_TRUE(to->inside(seq_t{N}));
    EXPECT_TRUE(to->inside(seq_t{2 * N - 1}));
    EXPECT_FALSE(to->inside(seq_t{2 * N}));

    // Boundary: N-1 is now below next_seq => retransmission
    EXPECT_TRUE(to->is_retransmission(seq_t{N - 1}));
    EXPECT_FALSE(to->is_retransmission(seq_t{N}));
}

TEST_F(TransportOutputTest, MultiSegmentReassemblyReordered) {
    to->insert({0}, make_frag({0}, true, false, 'A'), acb);
    to->insert({2}, make_frag({2}, false, true, 'C'), acb);

    // seq 0 drained, seq 2 in reorder buffer
    EXPECT_EQ(to->out.size(), 1u);
    EXPECT_TRUE(to->has_holes());

    to->insert({1}, make_frag({1}, false, false, 'B'), acb);
    // all 3 dgrams now in out
    EXPECT_EQ(to->out.size(), 3u);
    EXPECT_FALSE(to->has_holes());

    sgl msgl;
    auto rd = to->read(msgl);
    EXPECT_EQ(rd, 3);
    EXPECT_EQ(msgl.segs, 3u);
}

TEST_F(TransportOutputTest, MultiSegment) {
    auto *seg1 = make_frag({0}, true, false);
    auto next = slab->alloc_default(slab->kMaxDataLen);
    auto last = slab->alloc_default(slab->kMaxDataLen);
    seg1->next = next;
    next->next = last;

    auto seg2 = make_frag({1}, false, true);
    auto seg2last = slab->alloc_default(slab->kMaxDataLen);
    seg2->next = seg2last;

    to->insert({0}, seg1, acb);
    to->insert({1}, seg2, acb);

    EXPECT_EQ(to->out.size(), 2u);
    sgl msg;
    auto rd = to->read(msg);
    EXPECT_EQ(rd, 2);
    EXPECT_EQ(msg.segs, 5u);
    EXPECT_EQ(msg.size, slab->kMaxDataLen * 3 + 2);
}
