#include "test_env.h"

#include "msg_fragment.h"
#include "transport/protocol.h"
#include "transport/seq.h"
#include "transport/transport.h"
#include "transport/transport_output.h"

#include <bit>
#include <generic/rte_cycles.h>
#include <gtest/gtest.h>
#include <ranges>

class TransportOutputTest : public ::testing::Test {
protected:
  void SetUp() override {
    allocator = new msg_fragment_allocator("test_pool", 1023);
    to = new transport_output(allocator);
  }

  void TearDown() override {
    delete to;
    delete allocator;
  }

  msg_fragment *make_msg(seq_t seq, char payload, size_t size) {
    return make_frag(seq, true, true, payload, size);
  }

  msg_fragment *make_frag(seq_t seq, bool start, bool end, char payload = 'A', size_t size = 0) {
    auto *msg = allocator->alloc_msg_fragment(start * sizeof(protocol::ft_msg_payload) + sizeof(protocol::ft_header) + 1);
    EXPECT_NE(msg, nullptr);
    auto *hdr = msg->data<protocol::ft_header>();
    auto off = 0u;
    if(start){
        auto *mhdr = msg->data<protocol::ft_msg_payload>(sizeof(protocol::ft_header));
        mhdr->out = size;
        off += sizeof(protocol::ft_msg_payload);
    }
    hdr->type = protocol::pkt_type::FT_MSG;
    hdr->som = start;
    hdr->eom = end;
    hdr->seq = seq;
    *msg->get_ts() = 0;
    *msg->data<char>(sizeof(protocol::ft_header) + off) = payload;
    return msg;
  }

  msg_fragment *make_ctrl(seq_t seq) {
    auto *msg = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
    EXPECT_NE(msg, nullptr);
    auto *hdr = msg->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_WND_RET;
    hdr->seq = seq;
    *msg->get_ts() = 0;
    return msg;
  }

  msg_fragment_allocator *allocator;
  transport_output *to;
};

TEST_F(TransportOutputTest, Reordered) {
    std::vector<seq_t> seqs{{0}, {1}, {3}, {4}, {5}, {6}, {65}};
    std::vector<msg_fragment*> msgs;
    msgs.reserve(seqs.size());
    for(auto seq : seqs)
        if(seq == seq_t(1))
            msgs.emplace_back(make_ctrl(seq));
        else
            msgs.emplace_back(make_msg(seq, 'A', 1));
    for(auto [i, msg] : std::ranges::enumerate_view(msgs))
        to->insert(seqs[i], msg);
    EXPECT_EQ(to->out.size(), 1);
    protocol::ft_sack_payload py;
    to->copy_bitset(&py);
    ack_scheduler schdlr;
    EXPECT_TRUE(to->has_holes());
    EXPECT_EQ(std::popcount(py.bit_map[0]), 5);
    EXPECT_EQ(std::popcount(py.bit_map[1]), 0);
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(1));
    EXPECT_EQ(py.bit_map_len, 64);
    to->insert({2}, make_msg({2}, 'A', 1));
    EXPECT_EQ(to->out.size(), 6);
    to->copy_bitset(&py);
    EXPECT_EQ(py.bit_map[0], 1ull << 58);
}

TEST_F(TransportOutputTest, MultiSegmentReassembly) {
    to->insert({0}, make_frag({0}, true, false, 'A', 3));
    to->insert({1}, make_frag({1}, false, false, 'B'));
    to->insert({2}, make_frag({2}, false, true, 'C'));

    EXPECT_EQ(to->out.size(), 1);
    EXPECT_EQ(to->out.front().segs, 3);
    EXPECT_EQ(to->out.front().size, 3);
    to->insert({3}, make_msg({3}, 'A', 1));
    EXPECT_EQ(to->out.size(), 2u);
    EXPECT_EQ(to->out.back().segs, 1);
}

TEST_F(TransportOutputTest, ProactiveCreditReturnForBufferedMessage) {
    to->prepare_wnd_return();

    to->insert({0}, make_frag({0}, true, false, 'A', 4));
    to->insert({1}, make_frag({1}, false, false, 'B'));
    to->insert({2}, make_frag({2}, false, false, 'C'));

    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_EQ(to->reassembly.segs, 3u);
    EXPECT_EQ(to->reassembly.size, 4);

    char buf[64] = {};
    size_t remaining = 0;
    EXPECT_EQ(to->read(buf, sizeof(buf), remaining), 3);
    EXPECT_EQ(to->get_available_wnd(), 3u);
    EXPECT_EQ(to->reassembly.size, 1);

    // complete the msg_fragment with the final fragment
    to->insert({3}, make_frag({3}, false, true, 'D'));
    EXPECT_EQ(to->out.size(), 1u);
    EXPECT_EQ(to->out.front().segs, 1u);
    EXPECT_EQ(to->out.front().size, 1u);
    EXPECT_STREQ(buf, "ABC");

    auto prev_wnd = to->get_available_wnd();
    auto ret = to->read(buf + 3, 1, remaining);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(to->get_available_wnd(), prev_wnd + 1);
    EXPECT_STREQ(buf, "ABCD");
}

TEST_F(TransportOutputTest, InsertBoundary_ExactNextSeq) {
    // seq == next_seq should go directly to reassembly, not reorder buffer
    to->insert({0}, make_msg({0}, 'A', 1));
    EXPECT_EQ(to->out.size(), 1u);
    EXPECT_FALSE(to->rb.has_elements());
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(0));
}

TEST_F(TransportOutputTest, InsertBoundary_OneAboveNextSeq) {
    // seq == next_seq + 1 must go to reorder buffer, not reassembly
    to->insert({1}, make_msg({1}, 'B', 1));
    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_EQ(to->rb.next_buffered_seq(), seq_t(1));
}

TEST_F(TransportOutputTest, InsertBoundary_FillingGapDrainsReorderBuffer) {
    // Insert seq 1 and 2 out of order, then fill the gap with seq 0
    to->insert({1}, make_msg({1}, 'B', 1));
    to->insert({2}, make_msg({2}, 'C', 1));
    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_TRUE(to->rb.has_elements());

    to->insert({0}, make_msg({0}, 'A', 1));
    // All three should drain into out
    EXPECT_EQ(to->out.size(), 3u);
    EXPECT_FALSE(to->rb.has_elements());
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(2));
}

TEST_F(TransportOutputTest, InsertBoundary_GapNotFullyClosed) {
    // Insert 0, 1, 3 — gap at 2 means only 0 and 1 drain
    to->insert({0}, make_msg({0}, 'A', 1));
    to->insert({1}, make_msg({1}, 'B', 1));
    to->insert({3}, make_msg({3}, 'D', 1));
    EXPECT_EQ(to->out.size(), 2u);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(1));

    // Now fill the gap
    to->insert({2}, make_msg({2}, 'C', 1));
    EXPECT_EQ(to->out.size(), 4u);
    EXPECT_FALSE(to->rb.has_elements());
}

TEST_F(TransportOutputTest, InsertBoundary_LastValidSeqInWindow) {
    // seq == next_seq + kMaxBitMapSize - 1 is the last valid index
    constexpr auto last = transport_output::kMaxBitMapSize - 1;
    EXPECT_TRUE(to->inside(seq_t{last}));
    to->insert(seq_t{last}, make_msg(seq_t{last}, 'Z', 1));
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_EQ(to->rb.next_buffered_seq(), seq_t(last));
}

TEST_F(TransportOutputTest, InsertBoundary_ExceedsCapacityAtEdge) {
    // seq == next_seq + kMaxBitMapSize is out of window
    constexpr auto oob = transport_output::kMaxBitMapSize;
    EXPECT_FALSE(to->inside(seq_t{oob}));
    EXPECT_TRUE(to->exceeds_capacity(seq_t{oob}));
    // one before is still valid
    EXPECT_TRUE(to->inside(seq_t{oob - 1}));
    EXPECT_FALSE(to->exceeds_capacity(seq_t{oob - 1}));
}

TEST_F(TransportOutputTest, InsertBoundary_RetransmissionDetection) {
    // Insert and advance next_seq by processing seq 0
    to->insert({0}, make_msg({0}, 'A', 1));
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(0));

    // seq 0 is now < next_seq, should be detected as retransmission
    EXPECT_TRUE(to->is_retransmission(seq_t{0}));
    // seq 1 == next_seq, should NOT be a retransmission
    EXPECT_FALSE(to->is_retransmission(seq_t{1}));
}

TEST_F(TransportOutputTest, InsertBoundary_BitsetRetransmissionInWindow) {
    // Insert seq 0 and seq 2 (skip 1)
    to->insert({0}, make_msg({0}, 'A', 1));
    to->insert({2}, make_msg({2}, 'C', 1));
    // next_seq is now 1, seq 2 has its bit set in wnd
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(0));
    // seq 2 is in window and its bit is set => retransmission
    EXPECT_TRUE(to->is_retransmission(seq_t{2}));
    // seq 1 == next_seq, bit not set => not retransmission
    EXPECT_FALSE(to->is_retransmission(seq_t{1}));
    // seq 3 is in window, bit not set => not retransmission
    EXPECT_FALSE(to->is_retransmission(seq_t{3}));
}

TEST_F(TransportOutputTest, ReorderBuffer_InsertOrdering) {
    // Verify reorder buffer maintains sorted order with various insert patterns
    reorder_buffer rb;
    auto *m5 = make_msg({5}, 'E', 1);
    auto *m3 = make_msg({3}, 'C', 1);
    auto *m7 = make_msg({7}, 'G', 1);
    auto *m1 = make_msg({1}, 'A', 1);
    auto *m4 = make_msg({4}, 'D', 1);

    // Insert in non-sorted order
    rb.insert({5}, m5);
    rb.insert({3}, m3);  // before front
    rb.insert({7}, m7);  // after back
    rb.insert({1}, m1);  // new front
    rb.insert({4}, m4);  // middle, between 3 and 5

    // Drain and verify sorted order
    std::vector<uint32_t> order;
    while (rb.has_elements()) {
        order.push_back(rb.next_buffered_seq().v);
        rb.front()->free();
        rb.pop_front();
    }
    EXPECT_EQ(order, (std::vector<uint32_t>{1, 3, 4, 5, 7}));
}

TEST_F(TransportOutputTest, ReorderBuffer_InsertAtExactFrontAndBack) {
    reorder_buffer rb;
    auto *m5 = make_msg({5}, 'E', 1);
    auto *m3 = make_msg({3}, 'C', 1);
    auto *m10 = make_msg({10}, 'J', 1);

    rb.insert({5}, m5);
    // Insert exactly at < front boundary
    rb.insert({3}, m3);
    EXPECT_EQ(rb.next_buffered_seq(), seq_t(3));
    // Insert exactly at > back boundary
    rb.insert({10}, m10);

    std::vector<uint32_t> order;
    while (rb.has_elements()) {
        order.push_back(rb.next_buffered_seq().v);
        rb.front()->free();
        rb.pop_front();
    }
    EXPECT_EQ(order, (std::vector<uint32_t>{3, 5, 10}));
}

TEST_F(TransportOutputTest, InsertBoundary_FullWindow) {
    // Fill the entire 256-slot window out of order, then close the gap at seq 0
    constexpr auto N = transport_output::kMaxBitMapSize; // 256

    // Insert all slots except seq 0 (in forward order, all go to reorder buffer)
    for (uint32_t i = 1; i < N; ++i)
        to->insert(seq_t{i}, make_msg(seq_t{i}, 'A', 1));

    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_EQ(to->rb.next_buffered_seq(), seq_t(1));
    // next_seq is still 0
    EXPECT_EQ(to->get_last_rcvd_in_seq().v + 1, 0u);

    // Boundary: seq N-1 was the last valid slot, seq N would exceed capacity
    EXPECT_FALSE(to->inside(seq_t{N}));
    EXPECT_TRUE(to->exceeds_capacity(seq_t{N}));

    // Now insert seq 0 — should drain the entire reorder buffer
    to->insert(seq_t{0}, make_msg(seq_t{0}, 'A', 1));
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
    constexpr auto N = transport_output::kMaxBitMapSize;

    for (uint32_t i = N - 1; i >= 1; --i)
        to->insert(seq_t{i}, make_msg(seq_t{i}, 'A', 1));

    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_EQ(to->rb.next_buffered_seq(), seq_t(1));

    to->insert(seq_t{0}, make_msg(seq_t{0}, 'A', 1));
    EXPECT_EQ(to->out.size(), N);
    EXPECT_FALSE(to->rb.has_elements());
}

TEST_F(TransportOutputTest, InsertBoundary_FullWindowWithGaps) {
    // Fill even slots 0..254, leaving odd slots as gaps
    constexpr auto N = transport_output::kMaxBitMapSize;

    for (uint32_t i = 0; i < N; i += 2)
        to->insert(seq_t{i}, make_msg(seq_t{i}, 'A', 1));

    // Only seq 0 drained (next_seq advanced to 1, then hit gap at 1)
    EXPECT_EQ(to->out.size(), 1u);
    EXPECT_TRUE(to->rb.has_elements());
    EXPECT_TRUE(to->has_holes());
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(0));

    // Fill odd slots to close all gaps
    for (uint32_t i = 1; i < N; i += 2)
        to->insert(seq_t{i}, make_msg(seq_t{i}, 'A', 1));

    EXPECT_EQ(to->out.size(), N);
    EXPECT_FALSE(to->rb.has_elements());
    EXPECT_FALSE(to->has_holes());
}

TEST_F(TransportOutputTest, InsertBoundary_WindowSlideAfterFullDrain) {
    // Fill and drain window, then verify the window slides correctly
    constexpr auto N = transport_output::kMaxBitMapSize;

    // Fill in-order to avoid reorder buffer
    for (uint32_t i = 0; i < N; ++i)
        to->insert(seq_t{i}, make_msg(seq_t{i}, 'A', 1));

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
    to->insert({0}, make_frag({0}, true, false, 3, 'A'));
    to->insert({2}, make_frag({2}, false, true, 'C'));

    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_TRUE(to->has_holes());

    to->insert({1}, make_frag({1}, false, false, 'B'));
    EXPECT_EQ(to->out.size(), 1u);
    EXPECT_EQ(to->out.front().segs, 3);
    EXPECT_FALSE(to->has_holes());
}
