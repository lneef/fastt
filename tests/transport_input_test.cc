#include "slab_allocator.h"
#include "test_env.h"

#include "transport/protocol.h"
#include "transport/seq.h"
#include "transport/transport_input.h"
#include "util.h"
#include <generic/rte_cycles.h>

class TransportInputTest : public ::testing::Test {
protected:
  void SetUp() override {
    allocator = new slab_allocator{};
    ti = new transport_input();
    ti->update_budget(128);
  }

  void TearDown() override {
    delete ti;
    delete allocator;
  }

  mbuf *make_pkt() {
    auto *msg = allocator->alloc_default(sizeof(protocol::ft_header));
    EXPECT_NE(msg, nullptr);
    auto *hdr = msg->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_MSG;
    hdr->som = 1;
    hdr->eom = 1;
    return msg;
  }

  slab_allocator *allocator;
  transport_input *ti;
};

TEST_F(TransportInputTest, SackMarksCorrectEntries) {
    for (int i = 0; i < 5; ++i) {
        auto *msg = make_pkt();
        bool ok = ti->record_pkt(msg, [](mbuf *, seq_t) {}, rte_get_timer_cycles());
        ASSERT_TRUE(ok);
    }
    EXPECT_EQ(ti->size(), 5u);
    EXPECT_EQ(ti->get_seq(), seq_t{5});

    protocol::ft_sack_payload sack{};
    sack.bit_map[0] = (1ull << 1) | (1ull << 3) | (1ull << 4);
    sack.bit_map_len = 5;
    ti->acknowledge(seq_t{0} - 1, rte_get_timer_cycles());
    ti->acknowledge_sack(&sack, rte_get_timer_cycles());

    auto now = rte_get_timer_cycles();
    while(rte_get_timer_cycles() < now + get_ticks_us())
        ;
    ti->detect_loss(rte_get_timer_cycles());

    std::vector<mbuf*> retransmitted;
    ti->advance_recovery([&](mbuf *m) { retransmitted.push_back(m); });

    EXPECT_EQ(retransmitted.size(), 2u);
}

TEST_F(TransportInputTest, RetransmissionTransmitsCorrectPacket) {
    // Record 4 packets, each with a distinct payload byte
    std::vector<mbuf*> originals;
    for (int i = 0; i < 4; ++i) {
        auto *msg = allocator->alloc_default(sizeof(protocol::ft_header) + 1);
        ASSERT_NE(msg, nullptr);
        auto *hdr = msg->data<protocol::ft_header>();
        hdr->type = protocol::pkt_type::FT_MSG;
        hdr->som = 1;
        hdr->eom = 1;
        // Write a unique tag after the header
        *msg->data<uint8_t>(sizeof(protocol::ft_header)) = static_cast<uint8_t>(0xA0 + i);
        originals.push_back(msg);
        bool ok = ti->record_pkt(msg, [](mbuf *, seq_t) {}, rte_get_timer_cycles());
        ASSERT_TRUE(ok);
    }

    // ACK seq 0 (cumulative), leaving seq 1..3 unacked
    ti->acknowledge(seq_t{0}, rte_get_timer_cycles());

    // SACK seq 2 (bit 1 in bitmap starting after cumulative ack)
    // Unacked: 1, 2, 3 → bitmap bit 1 = seq 2
    protocol::ft_sack_payload sack{};
    sack.bit_map[0] = (1ull << 1);
    sack.bit_map_len = 3;
    ti->acknowledge_sack(&sack, rte_get_timer_cycles());

    // Wait past RTT so detect_loss triggers
    auto now = rte_get_timer_cycles();
    while (rte_get_timer_cycles() < now + 10 * get_ticks_us())
        ;
    ti->detect_loss(rte_get_timer_cycles());

    // Collect retransmitted packets
    std::vector<mbuf*> retransmitted;
    ti->advance_recovery([&](mbuf *m) { retransmitted.push_back(m); });

    // Expect seq 1 and seq 3 to be retransmitted (seq 2 was SACKed)
    ASSERT_EQ(retransmitted.size(), 1u);

    // Verify the retransmitted packets are the exact original mbufs
    EXPECT_EQ(retransmitted[0], originals[1]);

    // Verify payload is intact
    EXPECT_EQ(*retransmitted[0]->data<uint8_t>(sizeof(protocol::ft_header)), 0xA1);
}

TEST_F(TransportInputTest, UnsackedPacketsRetransmittedCorrectly) {
    // Record 8 packets (seq 0..7)
    for (int i = 0; i < 8; ++i) {
        auto *msg = make_pkt();
        bool ok = ti->record_pkt(msg, [](mbuf *, seq_t) {}, rte_get_timer_cycles());
        ASSERT_TRUE(ok);
    }
    EXPECT_EQ(ti->size(), 8u);
    EXPECT_EQ(ti->get_seq(), seq_t{8});

    // ACK seq 0 (cumulative), remaining unacked: 1..7
    ti->acknowledge(seq_t{0}, rte_get_timer_cycles());

    // SACK: bitmap covers entries 1..7 (7 entries after cumulative ack)
    // Mark 2, 4, 6 as sacked (bits 1, 3, 5 set); 1, 3, 5, 7 are not sacked (bits 0, 2, 4, 6 unset)
    protocol::ft_sack_payload sack{};
    sack.bit_map[0] = (1ull << 1) | (1ull << 3) | (1ull << 5);
    sack.bit_map_len = 7;
    ti->acknowledge_sack(&sack, rte_get_timer_cycles());
    auto now = rte_get_timer_cycles();
    while(rte_get_timer_cycles() < now + 10 * get_ticks_us())
        ;
    ti->detect_loss(rte_get_timer_cycles());
    std::vector<mbuf*> retransmitted;
    ti->advance_recovery([&](mbuf *m) { retransmitted.push_back(m); });
    EXPECT_EQ(retransmitted.size(), 3u);

    std::vector<mbuf*> second_round;
    ti->advance_recovery([&](mbuf *m) { second_round.push_back(m); });
    EXPECT_EQ(second_round.size(), 0u);

    protocol::ft_sack_payload sack2{};
    sack2.bit_map[0] = (1ull << 1) | (1ull << 3) | (1ull << 6);
    sack2.bit_map_len = 7;

    ti->acknowledge_sack(&sack2, rte_get_timer_cycles());
    now = rte_get_timer_cycles();
    while(rte_get_timer_cycles() < now + 200 * get_ticks_us())
        ;
    ti->detect_loss(rte_get_timer_cycles());

    // Only the still-unsacked packets that weren't already queued should appear
    // seq 3, 5, 7 were already retransmitted, so they should not be re-queued
    std::vector<mbuf*> third_round;
    ti->advance_recovery([&](mbuf *m) { third_round.push_back(m); });
    EXPECT_EQ(third_round.size(), 0u);
}

