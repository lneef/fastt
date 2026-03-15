#include "sgl.h"
#include "slab_allocator.h"
#include "task/task.h"

#include "transport/protocol.h"
#include "transport/seq.h"
#include "transport/transport.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <gtest/gtest.h>
#include <optional>

struct mock_packet_if {
  std::deque<mbuf *> sent_pkts;

  void consume_pkt_mbuf(mbuf *pkt, transport_config &, uint64_t ) {
    sent_pkts.push_back(pkt);
  }

  void consume_pkt_mbuf(mbuf *pkt, transport_config &) {
    sent_pkts.push_back(pkt);
  }

  void consume_for_retransmission(mbuf *msg) { sent_pkts.push_back(msg); }

  mbuf *pop() {
    if (sent_pkts.empty())
      return nullptr;
    auto *m = sent_pkts.front();
    sent_pkts.pop_front();
    return m;
  }

  uint32_t get_sip() const { return 0; }

  size_t size() const { return sent_pkts.size(); }

  void clear() { sent_pkts.clear(); }

  ~mock_packet_if() { clear(); }
};

// --- thin wrapper around transport<mock_packet_if> that mimics the
//     connection coro interface (make_progress / co_await send/recv) ---

using mock_transport = transport<mock_packet_if>;

struct mock_connection {
  mock_transport &tp;
  std::optional<concurrency::coro_handle> coro;

  mock_connection(mock_transport &tp) : tp(tp) {}

  void perform_recovery() {}

  // mirrors connection::make_progress
  void make_progress() { concurrency::make_progress(*this); }

  // --- awaitables that talk to mock_connection instead of connection ---

  concurrency::send_awaitable_sgl<mock_connection>
  send(concurrency::scheduler &s, sgl &&msgl) {
    return {s, *this, std::move(msgl)};
  }

  concurrency::recv_awaitable_sgl<mock_connection>
  recv(concurrency::scheduler &s, sgl *msgl) {
    return {s, *this, msgl};
  }

  bool can_send() const { return tp.can_send(); }

  bool can_recv() const { return tp.can_recv(); }

  ssize_t send_sgl(sgl &msgl) { return tp.send_sgl(msgl); }

  ssize_t recv(sgl &msgl) { return tp.recv(msgl); }
};

// --- test fixture ---

class TransportCoroTest : public ::testing::Test {
protected:
  static constexpr uint16_t kSport = 100;
  static constexpr uint16_t kDport = 200;

  void SetUp() override {
    slab = new slab_allocator{};
    mock = new mock_packet_if();
    cfg.ip = 0x01020304;
    cfg.transport_ports.sport = kSport;
    cfg.transport_ports.dport = kDport;
    tp = new mock_transport(mock, slab, cfg, kSport, kDport);
  }

  void TearDown() override {
    delete tp;
    delete mock;
    delete slab;
  }

  void establish() {
    auto *pkt = slab->alloc_default(sizeof(protocol::ft_header));
    auto *hdr = pkt->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_SYN;
    hdr->sport = kDport;
    hdr->dport = kSport;
    hdr->seq = {0};
    hdr->crd = 64;
    hdr->eom = true;
    hdr->ackframe = 0;
    hdr->sack = 0;
    tp->process_pkt(pkt);
    tp->accept_connection();
    ASSERT_TRUE(tp->up());
    mock->clear();
  }

  mbuf *make_data_pkt(seq_t seq, bool end, const void *payload,
                      uint16_t payload_len) {
    auto *msg = slab->alloc_default(payload_len);
    auto off = 0u;
    auto *hdr = msg->prepend<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_MSG;
    hdr->sport = kDport;
    hdr->dport = kSport;
    hdr->seq = seq;
    hdr->ack = {0};
    hdr->eom = end;
    hdr->ackframe = 1;
    hdr->sack = 0;
    hdr->crd = 0;
    std::memcpy(msg->data<uint8_t>(off + sizeof(protocol::ft_header)), payload,
                payload_len);
    return msg;
  }

  mbuf *make_crd_ret(seq_t seq, uint16_t crd) {
    auto *msg = slab->alloc_default(sizeof(protocol::ft_header));
    auto *hdr = msg->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_CRD_UPDATE;
    hdr->sport = kDport;
    hdr->dport = kSport;
    hdr->seq = seq;
    hdr->ack = {0};
    hdr->ackframe = 0;
    hdr->sack = 0;
    hdr->crd = crd;
    return msg;
  }

  slab_allocator *slab;
  mock_packet_if *mock;
  transport_config cfg;
  mock_transport *tp;
};

static concurrency::task recv_coro(concurrency::scheduler &sched,
                                   mock_connection &mc, sgl &msgl,
                                   ssize_t &out_retval) {
  out_retval = co_await mc.recv(sched, &msgl);
}

static concurrency::task send_coro(concurrency::scheduler &sched,
                                   mock_connection &mc, sgl &&msgl,
                                   ssize_t &out_retval) {
  out_retval = co_await mc.send(sched, std::move(msgl));
}

static concurrency::task recv_send_coro(concurrency::scheduler &sched,
                                        mock_connection &mc, slab_allocator *sb,
                                        sgl &recv_sgl, size_t rx) {
  auto rcvd = co_await mc.recv(sched, &recv_sgl);
  EXPECT_EQ(static_cast<ssize_t>(rx), rcvd);
  // Build an sgl from the received data to send back
  sgl send_sgl;
  for (auto &mb : recv_sgl) {
    printf("%u\n", mb.data_len);
    auto *m = sb->alloc_default(mb.data_len);
    std::memcpy(m->data<uint8_t>(), mb.data<void>(), mb.data_len);
    send_sgl.add_segment_safe(mbuf_take_owner_ship(m));
  }
  auto sent = co_await mc.send(sched, std::move(send_sgl));
  EXPECT_EQ(sent, rcvd);
}

TEST_F(TransportCoroTest, RecvSingleReady) {
  establish();

  const char payload[] = "hello";
  tp->process_pkt(make_data_pkt({1}, true, payload, sizeof(payload)));
  ASSERT_TRUE(tp->can_recv());

  concurrency::scheduler sched;
  mock_connection mc(*tp);

  sgl msgl;
  ssize_t retval = -1;
  auto t = recv_coro(sched, mc, msgl, retval);
  sched.schedule(t.handle);
  sched.run();

  EXPECT_GT(retval, 0);
  char buf[64] = {};
  msgl.head->read(buf);
  EXPECT_STREQ(buf, "hello");
}

TEST_F(TransportCoroTest, RecvSingleSuspendResume) {
  establish();

  concurrency::scheduler sched;
  mock_connection mc(*tp);

  sgl msgl;
  ssize_t retval = -1;

  auto t = recv_coro(sched, mc, msgl, retval);
  sched.schedule(t.handle);
  sched.run();
  EXPECT_EQ(retval, -1); // still suspended
  EXPECT_TRUE(mc.coro.has_value());

  const char payload[] = "world";
  tp->process_pkt(make_data_pkt({1}, true, payload, sizeof(payload)));

  // make_progress should resume the coro
  mc.make_progress();
  EXPECT_FALSE(mc.coro.has_value());
  sched.run(); // let the coro finish

  EXPECT_GT(retval, 0);
  char buf[64] = {};
  msgl.head->read(buf);
  EXPECT_STREQ(buf, "world");
}

// Multi-segment message (3 frags): all arrive, then recv via coro
TEST_F(TransportCoroTest, RecvMultiSegment) {
  establish();

  tp->process_pkt(make_data_pkt({1}, false, "AAA", 3));
  tp->process_pkt(make_data_pkt({2}, false, "BBB", 3));
  tp->process_pkt(make_data_pkt({3}, true, "CCC", 3));

  ASSERT_TRUE(tp->can_recv());

  concurrency::scheduler sched;
  mock_connection mc(*tp);

  sgl msgl;
  ssize_t retval = -1;
  auto t = recv_coro(sched, mc, msgl, retval);
  sched.schedule(t.handle);
  sched.run();

  EXPECT_EQ(retval, 9);
  char buf[64] = {};
  msgl.head->read(buf);
  EXPECT_EQ(std::memcmp(buf, "AAABBBCCC", 9), 0);
}

TEST_F(TransportCoroTest, SendViaCoro) {
  establish();
  mock->clear();

  concurrency::scheduler sched;
  mock_connection mc(*tp);

  char data[] = "testdata";
  sgl msgl;
  auto *m = slab->alloc_default(sizeof(data));
  std::memcpy(m->data<char>(), data, sizeof(data));
  msgl.add_segment_safe(mbuf_take_owner_ship(m));
  ssize_t retval = -1;

  auto t = send_coro(sched, mc, std::move(msgl), retval);
  sched.schedule(t.handle);
  sched.run();

  EXPECT_EQ(retval, static_cast<ssize_t>(sizeof(data)));
  ASSERT_GE(mock->size(), 1u);

  auto *sent = mock->pop();
  auto *fhdr = sent->data<protocol::ft_header>();
  EXPECT_EQ(fhdr->type, protocol::pkt_type::FT_MSG);
  EXPECT_TRUE(fhdr->eom);
}

// Two connections: recv from both, then send to both
TEST_F(TransportCoroTest, TwoConnectionsRecvThenSend) {
  // Set up second transport with different ports
  static constexpr uint16_t kSport2 = 300;
  static constexpr uint16_t kDport2 = 400;

  mock_packet_if mock2;
  transport_config cfg2;
  cfg2.ip = 0x05060708;
  cfg2.transport_ports.sport = kSport2;
  cfg2.transport_ports.dport = kDport2;
  mock_transport tp2(&mock2, slab, cfg2, kSport2, kDport2);

  // Establish both transports
  establish(); // tp1

  { // establish tp2
    auto *pkt = slab->alloc_default(sizeof(protocol::ft_header));
    auto *hdr = pkt->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_SYN;
    hdr->sport = kDport2;
    hdr->dport = kSport2;
    hdr->seq = {0};
    hdr->crd = 64;
    hdr->eom = true;
    hdr->ackframe = 0;
    hdr->sack = 0;
    tp2.process_pkt(pkt);
    tp2.accept_connection();
    ASSERT_TRUE(tp2.up());
    mock2.clear();
  }

  concurrency::scheduler sched;
  mock_connection mc1(*tp);
  mock_connection mc2(tp2);
  char payload1[] = "from_conn1";
  char payload2[] = "from_conn2";

  sgl rsgl1, rsgl2;

  auto t1 = recv_send_coro(sched, mc1, slab, rsgl1, sizeof(payload1));
  sched.schedule(t1.handle);
  auto t2 = recv_send_coro(sched, mc2, slab, rsgl2, sizeof(payload2));
  sched.schedule(t2.handle);
  sched.run();

  tp->process_pkt(make_data_pkt({1}, true, payload1, sizeof(payload1)));
  tp2.process_pkt(make_data_pkt({1}, true, payload2, sizeof(payload2)));

  mc1.make_progress();
  mc2.make_progress();
  sched.run();

  char buf1[64] = {}, buf2[64] = {};
  rsgl1.head->read(buf1);
  rsgl2.head->read(buf2);
  EXPECT_STREQ(buf1, "from_conn1");
  EXPECT_STREQ(buf2, "from_conn2");

  ASSERT_GE(mock->size(), 1u);
  ASSERT_GE(mock2.size(), 1u);

  auto *sent1 = mock->pop();
  auto *fhdr1 = sent1->data<protocol::ft_header>();
  EXPECT_EQ(fhdr1->type, protocol::pkt_type::FT_MSG);
  EXPECT_TRUE(fhdr1->eom);

  auto *sent2 = mock2.pop();
  auto *fhdr2 = sent2->data<protocol::ft_header>();
  EXPECT_EQ(fhdr2->type, protocol::pkt_type::FT_MSG);
  EXPECT_TRUE(fhdr2->eom);
}

// Two connections: coro1 recvs first, then coro2 recvs; coro1 is blocked on
// send (crd=0) with a large (3-segment) payload, coro2 sends freely; coro1
// gets crd=1 → partial send (1 segment), then crd=2 → remaining 2 segments.
TEST_F(TransportCoroTest, TwoConnectionsStaggeredRecvPartialWndReturn) {
  static constexpr uint16_t kSport2 = 300;
  static constexpr uint16_t kDport2 = 400;
  static constexpr size_t kSegSz = mock_transport::kMaxPayload;

  mock_packet_if mock2;
  transport_config cfg2;
  cfg2.ip = 0x05060708;
  cfg2.transport_ports.sport = kSport2;
  cfg2.transport_ports.dport = kDport2;
  mock_transport tp2(&mock2, slab, cfg2, kSport2, kDport2);

  // Establish tp1 with crd=0 — no send credits
  {
    auto *pkt = slab->alloc_default(sizeof(protocol::ft_header));
    auto *hdr = pkt->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_SYN;
    hdr->sport = kDport;
    hdr->dport = kSport;
    hdr->seq = {0};
    hdr->crd = 0;
    hdr->eom = true;
    hdr->ackframe = 0;
    hdr->sack = 0;
    tp->process_pkt(pkt);
    tp->accept_connection();
    ASSERT_TRUE(tp->up());
    ASSERT_FALSE(tp->can_send());
    mock->clear();
  }

  // Establish tp2 with crd=64 — plenty of send credits
  {
    auto *pkt = slab->alloc_default(sizeof(protocol::ft_header));
    auto *hdr = pkt->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_SYN;
    hdr->sport = kDport2;
    hdr->dport = kSport2;
    hdr->seq = {0};
    hdr->crd = 64;
    hdr->eom = true;
    hdr->ackframe = 0;
    hdr->sack = 0;
    tp2.process_pkt(pkt);
    tp2.accept_connection();
    ASSERT_TRUE(tp2.up());
    mock2.clear();
  }

  concurrency::scheduler sched;
  mock_connection mc1(*tp);
  mock_connection mc2(tp2);

  std::vector<char> big_payload(2 * kSegSz - 512, 'A');
  char payload2[] = "beta";

  sgl rsgl1, rsgl2;

  auto t1 = recv_send_coro(sched, mc1, slab, rsgl1, big_payload.size());
  sched.schedule(t1.handle);
  auto t2 = recv_send_coro(sched, mc2, slab, rsgl2, sizeof(payload2));
  sched.schedule(t2.handle);
  sched.run();

  EXPECT_TRUE(mc1.coro.has_value()); // suspended on recv
  EXPECT_TRUE(mc2.coro.has_value()); // suspended on recv

  tp->process_pkt(make_data_pkt({1}, false, big_payload.data(), kSegSz));

  mc1.make_progress();
  sched.run();
  tp->process_pkt(
      make_data_pkt({2}, true, big_payload.data() + kSegSz, kSegSz - 512));
  mc1.make_progress();
  sched.run();

  // coro1 completed recv, now suspended on send (crd=0 — can't send)
  EXPECT_TRUE(mc1.coro.has_value()); // blocked on send
  EXPECT_EQ(mock->size(), 0u);       // nothing sent yet

  // --- Phase 2: coro2 receives and sends freely (has credits) ---
  {
    char payload2[] = "beta";
    auto *dp2 = make_data_pkt({1}, true, payload2, sizeof(payload2));
    auto *dh2 = dp2->data<protocol::ft_header>();
    dh2->sport = kDport2;
    dh2->dport = kSport2;
    tp2.process_pkt(dp2);
  }
  mc2.make_progress();
  sched.run();

  char buf2[64] = {};
  rsgl2.head->read(buf2);
  EXPECT_STREQ(buf2, "beta");
  EXPECT_FALSE(mc2.coro.has_value()); // coro2 finished
  ASSERT_GE(mock2.size(), 1u);

  auto *sent2 = mock2.pop();
  auto *fhdr2 = sent2->data<protocol::ft_header>();
  EXPECT_EQ(fhdr2->type, protocol::pkt_type::FT_MSG);

  // coro1 still blocked
  EXPECT_TRUE(mc1.coro.has_value());
  EXPECT_EQ(mock->size(), 0u);

  // --- Phase 3: first crd_ret (crd=1) → coro1 sends 1 of 3 segments ---
  tp->process_pkt(make_crd_ret({3}, 1));
  mc1.make_progress();
  sched.run();

  // Partial send: 1 segment sent, coro still suspended
  EXPECT_TRUE(mc1.coro.has_value());
  ASSERT_EQ(mock->size(), 1u);

  auto *partial = mock->pop();
  auto *phdr = partial->data<protocol::ft_header>();
  EXPECT_EQ(phdr->type, protocol::pkt_type::FT_MSG);
  EXPECT_FALSE(phdr->eom); // not the last segment

  // --- Phase 4: second crd_ret (crd=2) → coro1 sends remaining 2 segments ---
  tp->process_pkt(make_crd_ret({4}, 1));
  mc1.make_progress();
  sched.run();

  EXPECT_FALSE(mc1.coro.has_value()); // coro1 done
  ASSERT_EQ(mock->size(), 1u);

  auto *seg2 = mock->pop();
  auto *shdr2 = seg2->data<protocol::ft_header>();
  EXPECT_EQ(shdr2->type, protocol::pkt_type::FT_MSG);
  EXPECT_TRUE(shdr2->eom);
}

TEST_F(TransportCoroTest, SendLargePayload) {
  // Establish with crd=2 so only 2 segments can be sent initially
  {
    auto *pkt = slab->alloc_default(sizeof(protocol::ft_header));
    auto *hdr = pkt->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_SYN;
    hdr->sport = kDport;
    hdr->dport = kSport;
    hdr->seq = {0};
    hdr->crd = 2;
    hdr->eom = true;
    hdr->ackframe = 0;
    hdr->sack = 0;
    tp->process_pkt(pkt);
    tp->accept_connection();
    ASSERT_TRUE(tp->up());
    mock->clear();
  }

  static constexpr size_t kPayloadSize = 16 * 1024; // 16 KB
  concurrency::scheduler sched;
  mock_connection mc(*tp);

  std::vector<char> payload(kPayloadSize);
  for (size_t i = 0; i < kPayloadSize; ++i)
    payload[i] = static_cast<char>('A' + (i % 26));

  sgl msgl;
  size_t off = 0;
  while (off < kPayloadSize) {
    auto to_cpy = std::min<size_t>(kPayloadSize - off, slab_allocator::kMaxDataLen);
    auto *m = slab->alloc_default(to_cpy);
    std::memcpy(m->data<char>(), payload.data() + off, to_cpy);
    msgl.add_segment_safe(mbuf_take_owner_ship(m));
    off += slab_allocator::kMaxDataLen;
  }
  ssize_t retval = -1;
  printf("%u\n", msgl.segs);

  auto t = send_coro(sched, mc, std::move(msgl), retval);
  sched.schedule(t.handle);
  sched.run();

  EXPECT_EQ(retval, -1);
  EXPECT_TRUE(mc.coro.has_value());
  ASSERT_EQ(mock->size(), 1u);

  std::vector<char> reassembled;
  while (mock->size()) {
    auto *pkt = mock->pop();
    auto *fhdr = pkt->data<protocol::ft_header>();
    EXPECT_EQ(fhdr->type, protocol::pkt_type::FT_MSG);
    EXPECT_FALSE(fhdr->eom); // more segments remain

    auto *seg_start = pkt->data<char>(sizeof(protocol::ft_header));
    size_t seg_len = pkt->data_len - sizeof(protocol::ft_header);
    reassembled.insert(reassembled.end(), seg_start, seg_start + seg_len);
  }

  // Grant enough credits for the remaining segments
  tp->process_pkt(make_crd_ret({3}, 64));
  mc.make_progress();
  sched.run();

  EXPECT_EQ(retval, static_cast<ssize_t>(kPayloadSize));
  EXPECT_FALSE(mc.coro.has_value());

  // The transport must have sent more segments
  ASSERT_GT(mock->size(), 0u);

  // Drain the remaining segments
  while (mock->size()) {
    auto *pkt = mock->pop();
    auto *fhdr = pkt->data<protocol::ft_header>();
    EXPECT_EQ(fhdr->type, protocol::pkt_type::FT_MSG);

    bool is_last = mock->size() == 0;
    if (is_last)
      EXPECT_TRUE(fhdr->eom);
    else
      EXPECT_FALSE(fhdr->eom);

    size_t meta = 0; // no ft_msg_payload on continuation segments
    auto *seg_start = pkt->data<char>(sizeof(protocol::ft_header) + meta);
    size_t seg_len = pkt->data_len - sizeof(protocol::ft_header) - meta;
    reassembled.insert(reassembled.end(), seg_start, seg_start + seg_len);
  }

  ASSERT_EQ(reassembled.size(), kPayloadSize);
  EXPECT_EQ(std::memcmp(reassembled.data(), payload.data(), kPayloadSize), 0);
}

// --- DONE acknowledgement tests ---

// Helper: build a FT_DONE packet that the remote would send to us.
static mbuf *make_done_pkt(slab_allocator *slab, seq_t seq, seq_t ack,
                           uint16_t sport, uint16_t dport) {
  auto *msg = slab->alloc_default(sizeof(protocol::ft_header));
  auto *hdr = msg->data<protocol::ft_header>();
  hdr->type = protocol::pkt_type::FT_DONE;
  hdr->sport = sport;
  hdr->dport = dport;
  hdr->seq = seq;
  hdr->ack = ack;
  hdr->ackframe = 0;
  hdr->sack = 0;
  hdr->crd = 0;
  hdr->eom = 0;
  return msg;
}

TEST_F(TransportCoroTest, SendAfterWndReturn) {
  auto *pkt = slab->alloc_default(sizeof(protocol::ft_header));
  auto *h = pkt->data<protocol::ft_header>();
  h->type = protocol::pkt_type::FT_SYN;
  h->sport = kDport;
  h->dport = kSport;
  h->seq = {0};
  h->crd = 0; // no send credits
  h->eom = true;
  h->ackframe = 0;
  h->sack = 0;
  tp->process_pkt(pkt);
  tp->accept_connection();
  ASSERT_TRUE(tp->up());
  mock->clear();

  ASSERT_FALSE(tp->can_send());

  concurrency::scheduler sched;
  mock_connection mc(*tp);

  char data[] = "x";
  sgl msgl;
  auto *m = slab->alloc_default(sizeof(data));
  std::memcpy(m->data<char>(), data, sizeof(data));
  msgl.add_segment_safe(mbuf_take_owner_ship(m));
  ssize_t retval = -1;

  // No credits — coro will suspend
  auto t = send_coro(sched, mc, std::move(msgl), retval);
  sched.schedule(t.handle);
  sched.run();
  EXPECT_EQ(retval, -1);
  EXPECT_TRUE(mc.coro.has_value());

  // Grant credits via WND_RET
  tp->process_pkt(make_crd_ret({1}, 8));

  mc.make_progress();
  EXPECT_FALSE(mc.coro.has_value());
  sched.run();

  EXPECT_EQ(retval, static_cast<ssize_t>(sizeof(data)));
  ASSERT_GE(mock->size(), 1u);
}
