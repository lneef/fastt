#include "slab_allocator.h"
#include "test_env.h"

#include "transport/congestion_control.h"
#include "transport/protocol.h"
#include "transport/seq.h"
#include "transport/transport_txpath.h"
#include "util.h"
#include <generic/rte_cycles.h>

class TransportInputTest : public ::testing::Test {
protected:
  void SetUp() override {
    allocator = new slab_allocator{};
    cc = new swift(rte_get_timer_hz());
    ti = new transport_txpath(*cc);
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
    hdr->eom = 1;
    return msg;
  }

  slab_allocator *allocator;
  swift *cc;
  transport_txpath *ti;
};

TEST_F(TransportInputTest, SackMarksCorrectEntries) {
    for (int i = 0; i < 5; ++i) {
        auto *msg = make_pkt();
        bool ok = ti->record_pkt(mbuf_take_owner_ship(msg), [](mbuf_ptr&, seq_t) {}, 0);
        ASSERT_TRUE(ok);
    }
    EXPECT_EQ(ti->size(), 5u);
    EXPECT_EQ(ti->get_seq(), seq_t{5});

    protocol::ft_sack_payload sack{};
    sack.bit_map[0] = (1ull << 1)  | (1ull << 3);
    sack.bit_map_len = 4;
    ti->acknowledge(seq_t{0} , rte_get_timer_cycles());
    ti->acknowledge_sack(&sack, rte_get_timer_cycles());

    auto now = rte_get_timer_cycles();
    while(rte_get_timer_cycles() < now + get_ticks_us())
        ;
    ti->detect_loss(rte_get_timer_cycles());

    std::vector<mbuf*> retransmitted;
    ti->advance_recovery([&](mbuf* m) { retransmitted.push_back(m); return true; }, rte_get_timer_cycles());

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
        hdr->eom = 1;
        // Write a unique tag after the header
        *msg->data<uint8_t>(sizeof(protocol::ft_header)) = static_cast<uint8_t>(0xA0 + i);
        originals.push_back(msg);
        bool ok = ti->record_pkt(mbuf_take_owner_ship(msg), [](mbuf_ptr&, seq_t) {}, rte_get_timer_cycles());
        ASSERT_TRUE(ok);
    }
    {
   auto now = rte_get_timer_cycles();
    while (rte_get_timer_cycles() < now + 100 * get_ticks_us())
        ;
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
    ti->advance_recovery([&](mbuf* m) { retransmitted.push_back(m); return true; }, rte_get_timer_cycles());

    // Expect seq 1 and seq 3 to be retransmitted (seq 2 was SACKed)
    ASSERT_EQ(retransmitted.size(), 1u);

    // Verify the retransmitted packets are the exact original mbufs
    EXPECT_EQ(retransmitted[0], originals[1]);

    // Verify payload is intact
    EXPECT_EQ(*retransmitted[0]->data<uint8_t>(sizeof(protocol::ft_header)), 0xA1);
}

TEST_F(TransportInputTest, CumulativeAckReturnsCrd) {
    // Budget starts at 128. Send 3 data packets (crd=1 each), then 1 ctrl
    // packet (crd=0), then 2 more data packets.
    // Seqs: 0(data) 1(data) 2(data) 3(ctrl) 4(data) 5(data)
    for (int i = 0; i < 3; ++i) {
        auto *msg = make_pkt();
        bool ok = ti->record_pkt(mbuf_take_owner_ship(msg), [](mbuf_ptr&, seq_t) {}, rte_get_timer_cycles());
        ASSERT_TRUE(ok);
    }
    EXPECT_EQ(ti->get_current_wnd(), 125u);

    // Control packet uses a seq but does not consume budget
    auto *ctrl = make_pkt();
    ti->record_ctrl_pkt(ctrl, [](mbuf*, seq_t) {}, rte_get_timer_cycles());
    EXPECT_EQ(ti->get_current_wnd(), 125u);

    for (int i = 0; i < 2; ++i) {
        auto *msg = make_pkt();
        bool ok = ti->record_pkt(mbuf_take_owner_ship(msg), [](mbuf_ptr&, seq_t) {}, rte_get_timer_cycles());
        ASSERT_TRUE(ok);
    }
    EXPECT_EQ(ti->get_current_wnd(), 123u);

    // Cumulative ACK through seq 3 returns crds for seq 0,1,2 (3 data) + seq 3 (ctrl, crd=0)
    ti->acknowledge(seq_t{3}, rte_get_timer_cycles());
    EXPECT_EQ(ti->get_current_wnd(), 126u);

    // Cumulative ACK through seq 5 returns remaining 2 data credits
    ti->acknowledge(seq_t{5}, rte_get_timer_cycles());
    EXPECT_EQ(ti->get_current_wnd(), 128u);
}

TEST_F(TransportInputTest, CumulativeAckReturnsCrdAfterSack) {
    // Send 4 packets (seq 0..3), consuming 4 credits from 128
    for (int i = 0; i < 4; ++i) {
        auto *msg = make_pkt();
        bool ok = ti->record_pkt(mbuf_take_owner_ship(msg), [](mbuf_ptr&, seq_t) {}, rte_get_timer_cycles());
        ASSERT_TRUE(ok);
    }
    EXPECT_EQ(ti->get_current_wnd(), 124u);

    // ACK seq 0 cumulatively — returns 1 crd
    ti->acknowledge(seq_t{0}, rte_get_timer_cycles());
    EXPECT_EQ(ti->get_current_wnd(), 125u);

    // SACK seq 2 (bit index 1 in unacked [1,2,3]) — no crd return from SACK
    protocol::ft_sack_payload sack{};
    sack.bit_map[0] = (1ull << 1);
    sack.bit_map_len = 3;
    ti->acknowledge_sack(&sack, rte_get_timer_cycles());
    EXPECT_EQ(ti->get_current_wnd(), 125u);

    // Cumulative ACK through seq 3 covers seq 1 (not sacked, crd=1),
    // seq 2 (sacked, crd=1), seq 3 (not sacked, crd=1) — all 3 crds returned
    ti->acknowledge(seq_t{3}, rte_get_timer_cycles());
    EXPECT_EQ(ti->get_current_wnd(), 128u);
}

TEST_F(TransportInputTest, CumulativeAckSeqWrapAround) {
    // Start near UINT32_MAX so sequences wrap around 0
    delete ti;
    ti = new transport_txpath(*cc, seq_t{UINT32_MAX - 2});
    ti->update_budget(128);

    // Send 6 packets: seqs MAX-2, MAX-1, MAX, 0, 1, 2
    for (int i = 0; i < 6; ++i) {
        auto *msg = make_pkt();
        bool ok = ti->record_pkt(mbuf_take_owner_ship(msg), [](mbuf_ptr&, seq_t) {}, rte_get_timer_cycles());
        ASSERT_TRUE(ok);
    }
    EXPECT_EQ(ti->size(), 6u);
    EXPECT_EQ(ti->get_seq(), seq_t{3});
    EXPECT_EQ(ti->get_current_wnd(), 122u);

    // Cumulative ACK through seq MAX (wraps across boundary), covers 3 packets
    ti->acknowledge(seq_t{UINT32_MAX}, rte_get_timer_cycles());
    EXPECT_EQ(ti->size(), 3u);
    EXPECT_EQ(ti->get_current_wnd(), 125u);

    // Cumulative ACK through seq 2 (post-wrap), covers remaining 3 packets
    ti->acknowledge(seq_t{2}, rte_get_timer_cycles());
    EXPECT_EQ(ti->size(), 0u);
    EXPECT_EQ(ti->get_current_wnd(), 128u);
}

TEST_F(TransportInputTest, SackSeqWrapAround) {
    // Start near UINT32_MAX so sequences wrap around 0
    delete ti;
    ti = new transport_txpath(*cc, seq_t{UINT32_MAX - 2});
    ti->update_budget(128);

    // Send 6 packets: seqs MAX-2, MAX-1, MAX, 0, 1, 2
    auto now = rte_get_timer_cycles(); 
    for (int i = 0; i < 6; ++i) {
        auto *msg = make_pkt();
        bool ok = ti->record_pkt(mbuf_take_owner_ship(msg), [](mbuf_ptr&, seq_t) {}, now);
        ASSERT_TRUE(ok);
    }
    EXPECT_EQ(ti->size(), 6u);
    EXPECT_EQ(ti->get_current_wnd(), 122u);

    {
        auto now = rte_get_timer_cycles();
        while (rte_get_timer_cycles() < now + 100 * get_ticks_us())
            ;
    }

    // Cumulative ACK seq MAX-2, leaving unacked: MAX-1, MAX, 0, 1, 2
    ti->acknowledge(seq_t{UINT32_MAX - 2}, rte_get_timer_cycles());
    EXPECT_EQ(ti->size(), 5u);
    EXPECT_EQ(ti->get_current_wnd(), 123u);

    // SACK bits 1 and 3 in unacked [MAX-1, MAX, 0, 1, 2] → marks MAX and 1
    protocol::ft_sack_payload sack{};
    sack.bit_map[0] = (1ull << 1) | (1ull << 3);
    sack.bit_map_len = 5;
    ti->acknowledge_sack(&sack, rte_get_timer_cycles());
    // SACK does not return credits
    EXPECT_EQ(ti->get_current_wnd(), 123u);

     now = rte_get_timer_cycles();
    while (rte_get_timer_cycles() < now + 10 * get_ticks_us())
        ;
    ti->detect_loss(rte_get_timer_cycles());

    // Unsacked packets: MAX-1 (bit 0), 0 (bit 2), 2 (bit 4) → 3 retransmitted
    std::vector<mbuf*> retransmitted;
    ti->advance_recovery([&](mbuf* m) { retransmitted.push_back(m); return true; }, rte_get_timer_cycles());
    EXPECT_EQ(retransmitted.size(), 2u);

    // Cumulative ACK through seq 2 covers everything, returns all remaining credits
    ti->acknowledge(seq_t{2}, rte_get_timer_cycles());
    EXPECT_EQ(ti->size(), 0u);
    EXPECT_EQ(ti->get_current_wnd(), 128u);
}

TEST_F(TransportInputTest, UnsackedPacketsRetransmittedCorrectly) {
    // Record 8 packets (seq 0..7)
    for (int i = 0; i < 8; ++i) {
        auto *msg = make_pkt();
        bool ok = ti->record_pkt(mbuf_take_owner_ship(msg), [](mbuf_ptr&, seq_t) {}, rte_get_timer_cycles());
        ASSERT_TRUE(ok);
    }
    EXPECT_EQ(ti->size(), 8u);
    EXPECT_EQ(ti->get_seq(), seq_t{8});
    {
   auto now = rte_get_timer_cycles();
    while (rte_get_timer_cycles() < now + 100 * get_ticks_us())
        ;
    }

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
    ti->advance_recovery([&](mbuf *m) { retransmitted.push_back(m); return true; }, rte_get_timer_cycles());
    EXPECT_EQ(retransmitted.size(), 3u);

    std::vector<mbuf*> second_round;
    ti->advance_recovery([&](mbuf* m) { second_round.push_back(m); return true; }, rte_get_timer_cycles());
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
    ti->advance_recovery([&](mbuf* m) { third_round.push_back(m); return true; }, rte_get_timer_cycles());
    EXPECT_EQ(third_round.size(), 0u);
}

