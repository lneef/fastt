#include "task/task.h"
#include "test_env.h"

#include "msg_fragment.h"
#include "transport/protocol.h"
#include "transport/seq.h"
#include "transport/transport.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <gtest/gtest.h>
#include <optional>

struct mock_packet_if {
  std::deque<msg_fragment *> sent_pkts;

  void consume_pkt(msg_fragment *pkt, transport_config &) {
    pkt->inc_refcnt();
    sent_pkts.push_back(pkt);
  }

  void consume_for_retransmission(msg_fragment *msg) {
    msg->inc_refcnt();
    sent_pkts.push_back(msg);
  }

  msg_fragment *pop() {
    if (sent_pkts.empty())
      return nullptr;
    auto *m = sent_pkts.front();
    sent_pkts.pop_front();
    return m;
  }

  size_t size() const { return sent_pkts.size(); }

  void clear() {
    for (auto *m : sent_pkts)
      m->free();
    sent_pkts.clear();
  }

  ~mock_packet_if() { clear(); }
};

// --- thin wrapper around transport<mock_packet_if> that mimics the
//     connection coro interface (make_progress / co_await send/recv) ---

using mock_transport = transport<mock_packet_if>;

struct mock_connection {
  mock_transport &tp;
  std::optional<concurrency::coro_handle> coro;

  mock_connection(mock_transport &tp) : tp(tp) {}

  // mirrors connection::make_progress
  void make_progress() {
    concurrency::make_progress(*this);  
  }

  // --- awaitables that talk to mock_connection instead of connection ---
 
  concurrency::send_awaitable<mock_connection> send(concurrency::scheduler &s, msg_hdr &hdr) {
    return {s, *this, hdr};
  }

  concurrency::recv_awaitable<mock_connection> recv(concurrency::scheduler &s, void *buf, size_t len,
                      size_t &remaining) {
    return {s, *this, buf, len, remaining};
  }

  bool can_send() const{
      return tp.can_send();
  }

  bool can_recv() const{
      return tp.can_recv();
  }

  ssize_t send(msg_hdr &hdr){
      return tp.send(hdr);
  }

  ssize_t recv(void* buf, size_t len, size_t& rem){
      return tp.recv(buf, len, rem);
  }
};

// --- test fixture ---

class TransportCoroTest : public ::testing::Test {
protected:
  static constexpr uint16_t kSport = 100;
  static constexpr uint16_t kDport = 200;

  void SetUp() override {
    allocator = new msg_fragment_allocator("coro_test_pool", 1023);
    mock = new mock_packet_if();
    cfg.ip = 0x01020304;
    cfg.transport_ports.sport = kSport;
    cfg.transport_ports.dport = kDport;
    tp = new mock_transport(allocator, mock, cfg, kSport, kDport);
  }

  void TearDown() override {
    delete tp;
    delete mock;
    delete allocator;
  }

  void establish() {
    auto *pkt = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
    auto *hdr = pkt->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_RDY_TO_RCV;
    hdr->sport = kDport;
    hdr->dport = kSport;
    hdr->seq = {0};
    hdr->wnd = 64;
    hdr->start = true;
    hdr->end = true;
    hdr->ackframe = 0;
    hdr->sack = 0;
    hdr->ts = 0;
    *pkt->get_ts() = 0;
    tp->process_pkt(pkt);
    ASSERT_TRUE(tp->up());
    mock->clear();
  }

  msg_fragment *make_data_pkt(seq_t seq, bool start, bool end, const void *payload,
                         uint16_t payload_len) {
    auto *msg =
        allocator->alloc_msg_fragment(sizeof(protocol::ft_header) + payload_len);
    auto *hdr = msg->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_MSG;
    hdr->sport = kDport;
    hdr->dport = kSport;
    hdr->seq = seq;
    hdr->ack = {0};
    hdr->start = start;
    hdr->end = end;
    hdr->ackframe = 0;
    hdr->sack = 0;
    hdr->wnd = 0;
    hdr->ts = 0;
    std::memcpy(msg->data<uint8_t>() + sizeof(protocol::ft_header), payload,
                payload_len);
    *msg->get_ts() = 0;
    return msg;
  }

  msg_fragment *make_wnd_ret(seq_t seq, uint16_t wnd) {
    auto *msg = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
    auto *hdr = msg->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_WND_RET;
    hdr->sport = kDport;
    hdr->dport = kSport;
    hdr->seq = seq;
    hdr->ack = {0};
    hdr->start = true;
    hdr->end = true;
    hdr->ackframe = 0;
    hdr->sack = 0;
    hdr->wnd = wnd;
    hdr->ts = 0;
    *msg->get_ts() = 0;
    return msg;
  }

  msg_fragment_allocator *allocator;
  mock_packet_if *mock;
  transport_config cfg;
  mock_transport *tp;
};

static concurrency::task recv_coro(concurrency::scheduler &sched,
                                   mock_connection &mc, void *buf, size_t len,
                                   size_t &remaining, ssize_t &out_retval) {
  out_retval = co_await mc.recv(sched, buf, len, remaining);
}

static concurrency::task send_coro(concurrency::scheduler &sched,
                                   mock_connection &mc, msg_hdr &hdr,
                                   ssize_t &out_retval) {
  out_retval = co_await mc.send(sched, hdr);
}

static concurrency::task recv_send_coro(concurrency::scheduler &sched,
                                        mock_connection &mc, msg_hdr &hdr,
                                        std::vector<char>& data, size_t rem) {
  auto rcvd = co_await mc.recv(sched, data.data(), data.size(), rem);
  hdr.set_data(data.data(), rcvd);
  auto sent = co_await mc.send(sched, hdr);
  EXPECT_EQ(sent, rcvd);
}

TEST_F(TransportCoroTest, RecvSingleReady) {
  establish();

  const char payload[] = "hello";
  tp->process_pkt(make_data_pkt({1}, true, true, payload, sizeof(payload)));
  ASSERT_TRUE(tp->can_recv());

  concurrency::scheduler sched;
  mock_connection mc(*tp);

  char buf[64] = {};
  size_t remaining = 0;
  ssize_t retval = -1;
  auto t = recv_coro(sched, mc, buf, sizeof(buf), remaining, retval);
  sched.schedule(t.handle);
  sched.run();

  EXPECT_GT(retval, 0);
  EXPECT_EQ(remaining, 0u);
  EXPECT_STREQ(buf, "hello");
}

TEST_F(TransportCoroTest, RecvSingleSuspendResume) {
  establish();

  concurrency::scheduler sched;
  mock_connection mc(*tp);

  char buf[64] = {};
  size_t remaining = 0;
  ssize_t retval = -1;

  auto t = recv_coro(sched, mc, buf, sizeof(buf), remaining, retval);
  sched.schedule(t.handle);
  sched.run();
  EXPECT_EQ(retval, -1); // still suspended
  EXPECT_TRUE(mc.coro.has_value());

  const char payload[] = "world";
  tp->process_pkt(make_data_pkt({1}, true, true, payload, sizeof(payload)));

  // make_progress should resume the coro
  mc.make_progress();
  EXPECT_FALSE(mc.coro.has_value());
  sched.run(); // let the coro finish

  EXPECT_GT(retval, 0);
  EXPECT_STREQ(buf, "world");
}

// Multi-segment msg_fragment (3 frags): all arrive, then recv via coro
TEST_F(TransportCoroTest, RecvMultiSegment) {
  establish();

  tp->process_pkt(make_data_pkt({1}, true, false, "AAA", 3));
  tp->process_pkt(make_data_pkt({2}, false, false, "BBB", 3));
  tp->process_pkt(make_data_pkt({3}, false, true, "CCC", 3));

  ASSERT_TRUE(tp->can_recv());

  concurrency::scheduler sched;
  mock_connection mc(*tp);

  char buf[64] = {};
  size_t remaining = 0;
  ssize_t retval = -1;
  auto t = recv_coro(sched, mc, buf, sizeof(buf), remaining, retval);
  sched.schedule(t.handle);
  sched.run();

  EXPECT_EQ(retval, 9);
  EXPECT_EQ(remaining, 0u);
  EXPECT_EQ(std::memcmp(buf, "AAABBBCCC", 9), 0);
}

TEST_F(TransportCoroTest, SendViaCoro) {
  establish();
  mock->clear();

  concurrency::scheduler sched;
  mock_connection mc(*tp);

  char data[] = "testdata";
  msg_hdr hdr;
  hdr.set_data(data, sizeof(data));
  ssize_t retval = -1;

  auto t = send_coro(sched, mc, hdr, retval);
  sched.schedule(t.handle);
  sched.run();

  EXPECT_EQ(retval, static_cast<ssize_t>(sizeof(data)));
  ASSERT_GE(mock->size(), 1u);

  auto *sent = mock->pop();
  auto *fhdr = sent->data<protocol::ft_header>();
  EXPECT_EQ(fhdr->type, protocol::pkt_type::FT_MSG);
  EXPECT_TRUE(fhdr->start);
  EXPECT_TRUE(fhdr->end);
  sent->free();
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
  mock_transport tp2(allocator, &mock2, cfg2, kSport2, kDport2);

  // Establish both transports
  establish(); // tp1

  { // establish tp2
    auto *pkt = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
    auto *hdr = pkt->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_RDY_TO_RCV;
    hdr->sport = kDport2;
    hdr->dport = kSport2;
    hdr->seq = {0};
    hdr->wnd = 64;
    hdr->start = true;
    hdr->end = true;
    hdr->ackframe = 0;
    hdr->sack = 0;
    hdr->ts = 0;
    *pkt->get_ts() = 0;
    tp2.process_pkt(pkt);
    ASSERT_TRUE(tp2.up());
    mock2.clear();
  }

  concurrency::scheduler sched;
  mock_connection mc1(*tp);
  mock_connection mc2(tp2);
  std::vector<char> buf1(64), buf2(64);
  char payload1[] = "from_conn1";
  char payload2[] = "from_conn2"; 

  size_t rem1 = 0, rem2 = 0;
  msg_hdr hdr1, hdr2;

  auto t1 = recv_send_coro(sched, mc1, hdr1, buf1, rem1);
  sched.schedule(t1.handle);
  auto t2 = recv_send_coro(sched, mc2, hdr2, buf2, rem2);
  sched.schedule(t2.handle);
  sched.run();

  tp->process_pkt(make_data_pkt({1}, true, true, payload1, sizeof(payload1)));
  tp2.process_pkt(make_data_pkt({1}, true, true, payload2, sizeof(payload2)));

  mc1.make_progress();
  mc2.make_progress();
  sched.run();

  EXPECT_EQ(rem1, 0u);
  EXPECT_STREQ(buf1.data(), "from_conn1");

  EXPECT_EQ(rem2, 0u);
  EXPECT_STREQ(buf2.data(), "from_conn2");

  ASSERT_GE(mock->size(), 1u);
  ASSERT_GE(mock2.size(), 1u);

  auto *sent1 = mock->pop();
  auto *fhdr1 = sent1->data<protocol::ft_header>();
  EXPECT_EQ(fhdr1->type, protocol::pkt_type::FT_MSG);
  EXPECT_TRUE(fhdr1->start);
  EXPECT_TRUE(fhdr1->end);
  sent1->free();

  auto *sent2 = mock2.pop();
  auto *fhdr2 = sent2->data<protocol::ft_header>();
  EXPECT_EQ(fhdr2->type, protocol::pkt_type::FT_MSG);
  EXPECT_TRUE(fhdr2->start);
  EXPECT_TRUE(fhdr2->end);
  sent2->free();
}

// Two connections: coro1 recvs first, then coro2 recvs; coro1 is blocked on
// send (wnd=0) with a large (3-segment) payload, coro2 sends freely; coro1
// gets wnd=1 → partial send (1 segment), then wnd=2 → remaining 2 segments.
TEST_F(TransportCoroTest, TwoConnectionsStaggeredRecvPartialWndReturn) {
  static constexpr uint16_t kSport2 = 300;
  static constexpr uint16_t kDport2 = 400;
  static constexpr size_t kSegSz = mock_transport::kMaxPayload;

  mock_packet_if mock2;
  transport_config cfg2;
  cfg2.ip = 0x05060708;
  cfg2.transport_ports.sport = kSport2;
  cfg2.transport_ports.dport = kDport2;
  mock_transport tp2(allocator, &mock2, cfg2, kSport2, kDport2);

  // Establish tp1 with wnd=0 — no send credits
  {
    auto *pkt = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
    auto *hdr = pkt->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_RDY_TO_RCV;
    hdr->sport = kDport;
    hdr->dport = kSport;
    hdr->seq = {0};
    hdr->wnd = 0;
    hdr->start = true;
    hdr->end = true;
    hdr->ackframe = 0;
    hdr->sack = 0;
    hdr->ts = 0;
    *pkt->get_ts() = 0;
    tp->process_pkt(pkt);
    ASSERT_TRUE(tp->up());
    ASSERT_FALSE(tp->can_send());
    mock->clear();
  }

  // Establish tp2 with wnd=64 — plenty of send credits
  {
    auto *pkt = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
    auto *hdr = pkt->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_RDY_TO_RCV;
    hdr->sport = kDport2;
    hdr->dport = kSport2;
    hdr->seq = {0};
    hdr->wnd = 64;
    hdr->start = true;
    hdr->end = true;
    hdr->ackframe = 0;
    hdr->sack = 0;
    hdr->ts = 0;
    *pkt->get_ts() = 0;
    tp2.process_pkt(pkt);
    ASSERT_TRUE(tp2.up());
    mock2.clear();
  }

  concurrency::scheduler sched;
  mock_connection mc1(*tp);
  mock_connection mc2(tp2);

  std::vector<char> big_payload(2 * kSegSz - 512, 'A');
  std::vector<char> buf1(2 * kSegSz - 512), buf2(64);
  size_t rem1 = 0, rem2 = 0;
  msg_hdr hdr1, hdr2;

  auto t1 = recv_send_coro(sched, mc1, hdr1, buf1, rem1);
  sched.schedule(t1.handle);
  auto t2 = recv_send_coro(sched, mc2, hdr2, buf2, rem2);
  sched.schedule(t2.handle);
  sched.run();

  EXPECT_TRUE(mc1.coro.has_value()); // suspended on recv
  EXPECT_TRUE(mc2.coro.has_value()); // suspended on recv

  tp->process_pkt(make_data_pkt({1}, true, false, big_payload.data(), kSegSz));
  tp->process_pkt(
      make_data_pkt({2}, false, true, big_payload.data() + kSegSz, kSegSz - 512));

  mc1.make_progress();
  sched.run();

  // coro1 completed recv, now suspended on send (wnd=0 — can't send)
  EXPECT_EQ(rem1, 0u);
  EXPECT_TRUE(mc1.coro.has_value()); // blocked on send
  EXPECT_EQ(mock->size(), 0u);       // nothing sent yet

  // --- Phase 2: coro2 receives and sends freely (has credits) ---
  {
    char payload2[] = "beta";
    auto *dp2 = make_data_pkt({1}, true, true, payload2, sizeof(payload2));
    auto *dh2 = dp2->data<protocol::ft_header>();
    dh2->sport = kDport2;
    dh2->dport = kSport2;
    tp2.process_pkt(dp2);
  }
  mc2.make_progress();
  sched.run();

  EXPECT_STREQ(buf2.data(), "beta");
  EXPECT_FALSE(mc2.coro.has_value()); // coro2 finished
  ASSERT_GE(mock2.size(), 1u);

  auto *sent2 = mock2.pop();
  auto *fhdr2 = sent2->data<protocol::ft_header>();
  EXPECT_EQ(fhdr2->type, protocol::pkt_type::FT_MSG);
  sent2->free();

  // coro1 still blocked
  EXPECT_TRUE(mc1.coro.has_value());
  EXPECT_EQ(mock->size(), 0u);

  // --- Phase 3: first wnd_ret (wnd=1) → coro1 sends 1 of 3 segments ---
  tp->process_pkt(make_wnd_ret({3}, 1));
  mc1.make_progress();
  sched.run();

  // Partial send: 1 segment sent, coro still suspended
  EXPECT_TRUE(mc1.coro.has_value());
  ASSERT_EQ(mock->size(), 1u);

  auto *partial = mock->pop();
  auto *phdr = partial->data<protocol::ft_header>();
  EXPECT_EQ(phdr->type, protocol::pkt_type::FT_MSG);
  EXPECT_TRUE(phdr->start);
  EXPECT_FALSE(phdr->end); // not the last segment
  partial->free();

  // --- Phase 4: second wnd_ret (wnd=2) → coro1 sends remaining 2 segments ---
  tp->process_pkt(make_wnd_ret({4}, 1));
  mc1.make_progress();
  sched.run();

  EXPECT_FALSE(mc1.coro.has_value()); // coro1 done
  ASSERT_EQ(mock->size(), 1u);

  auto *seg2 = mock->pop();
  auto *shdr2 = seg2->data<protocol::ft_header>();
  EXPECT_EQ(shdr2->type, protocol::pkt_type::FT_MSG);
  EXPECT_FALSE(shdr2->start);
  EXPECT_TRUE(shdr2->end);
  seg2->free();
}

TEST_F(TransportCoroTest, SendAfterWndReturn) {
  // Establish with 0 extra credits by using wnd=1 (consumed by init ctrl pkt)
  auto *pkt = allocator->alloc_msg_fragment(sizeof(protocol::ft_header));
  auto *h = pkt->data<protocol::ft_header>();
  h->type = protocol::pkt_type::FT_RDY_TO_RCV;
  h->sport = kDport;
  h->dport = kSport;
  h->seq = {0};
  h->wnd = 0; // no send credits
  h->start = true;
  h->end = true;
  h->ackframe = 0;
  h->sack = 0;
  h->ts = 0;
  *pkt->get_ts() = 0;
  tp->process_pkt(pkt);
  ASSERT_TRUE(tp->up());
  mock->clear();

  ASSERT_FALSE(tp->can_send());

  concurrency::scheduler sched;
  mock_connection mc(*tp);

  char data[] = "x";
  msg_hdr hdr;
  hdr.set_data(data, sizeof(data));
  ssize_t retval = -1;

  // No credits — coro will suspend
  auto t = send_coro(sched, mc, hdr, retval);
  sched.schedule(t.handle);
  sched.run();
  EXPECT_EQ(retval, -1);
  EXPECT_TRUE(mc.coro.has_value());

  // Grant credits via WND_RET
  tp->process_pkt(make_wnd_ret({1}, 8));

  mc.make_progress();
  EXPECT_FALSE(mc.coro.has_value());
  sched.run();

  EXPECT_EQ(retval, static_cast<ssize_t>(sizeof(data)));
  ASSERT_GE(mock->size(), 1u);
}
