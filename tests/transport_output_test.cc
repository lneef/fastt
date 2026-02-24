#include "test_env.h"

#include "message.h"
#include "transport/protocol.h"
#include "transport/seq.h"
#include "transport/transport_output.h"

#include <algorithm>
#include <bit>
#include <ranges>

class TransportOutputTest : public ::testing::Test {
protected:
  void SetUp() override {
    allocator = new message_allocator("test_pool", 1023);
    to = new transport_output(allocator);
  }

  void TearDown() override {
    delete to;
    delete allocator;
  }

  message *make_msg(seq_t seq) {
    return make_frag(seq, true, true);
  }

  message *make_frag(seq_t seq, bool start, bool end) {
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    EXPECT_NE(msg, nullptr);
    auto *hdr = msg->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_MSG;
    hdr->start = start;
    hdr->end = end;
    hdr->seq = seq;
    *msg->get_ts() = 0;
    return msg;
  }

  message_allocator *allocator;
  transport_output *to;
};

TEST_F(TransportOutputTest, Reordered) {
    std::vector<seq_t> seqs{{0}, {2}, {3}, {4}, {5}, {6}, {65}};
    std::vector<message*> msgs;
    msgs.reserve(seqs.size());
    for(auto seq : seqs | std::ranges::views::reverse)
        msgs.emplace_back(make_msg(seq));
    for(auto [i, msg] : std::ranges::enumerate_view(msgs))
        to->insert(seqs[i], msg);
    EXPECT_EQ(to->out.size(), 1);
    protocol::ft_sack_payload py;
    to->copy_bitset(&py);
    EXPECT_EQ(std::popcount(py.bit_map[0]), 5);
    EXPECT_EQ(std::popcount(py.bit_map[1]), 1);
    EXPECT_EQ(to->get_last_rcvd_in_seq(), seq_t(0));
    EXPECT_EQ(py.bit_map_len, 65);
    to->insert({1}, make_msg({1}));
    EXPECT_EQ(to->out.size(), 7);
    to->copy_bitset(&py);
    EXPECT_EQ(py.bit_map[0], 1ull << 58);
}

TEST_F(TransportOutputTest, MultiSegmentReassembly) {
    to->insert({0}, make_frag({0}, true, false));
    to->insert({1}, make_frag({1}, false, false));
    to->insert({2}, make_frag({2}, false, true));

    EXPECT_EQ(to->out.size(), 1);
    auto *msg = to->out.front();
    EXPECT_EQ(msg->nb_segs, 3);

    to->insert({3}, make_msg({3}));
    EXPECT_EQ(to->out.size(), 2u);
    EXPECT_EQ(to->out.back()->nb_segs, 1);
}

TEST_F(TransportOutputTest, MultiSegmentReassemblyReordered) {
    to->insert({0}, make_frag({0}, true, false));
    to->insert({2}, make_frag({2}, false, true));

    EXPECT_EQ(to->out.size(), 0u);
    EXPECT_TRUE(to->has_holes());

    to->insert({1}, make_frag({1}, false, false));
    EXPECT_EQ(to->out.size(), 1u);
    EXPECT_EQ(to->out.front()->nb_segs, 3);
    EXPECT_FALSE(to->has_holes());
}
