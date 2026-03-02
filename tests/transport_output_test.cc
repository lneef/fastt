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
