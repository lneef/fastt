#include "test_env.h"

#include "msg_fragment.h"
#include "transport/protocol.h"
#include "transport/seq.h"
#include "transport/transport.h"
#include "transport/transport_input.h"
#include "util.h"

class TransportInputTest : public ::testing::Test {
protected:
  void SetUp() override {
    allocator = new msg_fragment_allocator("ti_pool", 1023);
    ti = new transport_input();
    ti->update_budget(128);
  }

  void TearDown() override {
    delete ti;
    delete allocator;
  }

  msg_fragment *make_pkt() {
    auto *msg = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
    EXPECT_NE(msg, nullptr);
    auto *hdr = msg->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_MSG;
    hdr->start = 1;
    hdr->end = 1;
    *msg->get_ts() = 0;
    return msg;
  }

  msg_fragment_allocator *allocator;
  transport_input *ti;
};

TEST_F(TransportInputTest, SackMarksCorrectEntries) {
    for (int i = 0; i < 5; ++i) {
        auto *msg = make_pkt();
        bool ok = ti->record_pkt(msg, [](msg_fragment *, seq_t) {});
        ASSERT_TRUE(ok);
    }
    EXPECT_EQ(ti->size(), 5u);
    EXPECT_EQ(ti->get_seq(), seq_t{5});

    protocol::ft_sack_payload sack{};
    sack.bit_map[0] = (1ull << 1) | (1ull << 3) | (1ull << 4);
    sack.bit_map_len = 5;

    std::vector<seq_t> retransmitted;
    ti->acknowledge(seq_t{0} - 1, 0, true);
    ti->acknowledge_sack(&sack, 0,
        [&](msg_fragment *) { retransmitted.push_back({}); });

    EXPECT_EQ(retransmitted.size(), 2u);
}


TEST(AckSchedulerTest, SackRateLimitedByRtt) {
    ack_scheduler sched;
    uint64_t rtt = 100;
    auto us = get_ticks_us();
    auto now = us * 1000;

    EXPECT_TRUE(sched.sack_pending(1, now, rtt));
    sched.sack_callback(seq_t{0}, 1, now);

    now += 25 * us;
    EXPECT_FALSE(sched.sack_pending(1, now, rtt));

    now += 10 * us;
    EXPECT_FALSE(sched.sack_pending(2, now, rtt));

    now += 15 * us;
    EXPECT_TRUE(sched.sack_pending(2, now, rtt));

    sched.sack_callback(seq_t{1}, 2, now);

    now += 5 * us;
    EXPECT_FALSE(sched.sack_pending(3, now, rtt));

    now += 50 * us;
    EXPECT_TRUE(sched.sack_pending(3, now, rtt));
}

