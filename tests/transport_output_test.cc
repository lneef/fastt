#include "message.h"
#include "transport/protocol.h"
#include "transport/seq.h"
#include "transport/transport_output.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <rte_eal.h>

class DpdkEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    const char *argv[] = {"test", "--no-huge", "--log-level=0"};
    int argc = sizeof(argv) / sizeof(argv[0]);
    int ret = rte_eal_init(argc, const_cast<char **>(argv));
    ASSERT_GE(ret, 0) << "Failed to initialize DPDK EAL";
    ASSERT_EQ(message::init(), 0) << "Failed to register timestamp dynfield";
  }
};

testing::Environment *const dpdk_env =
    testing::AddGlobalTestEnvironment(new DpdkEnvironment);

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
    auto *msg = allocator->alloc_message(sizeof(protocol::ft_header));
    EXPECT_NE(msg, nullptr);
    auto *hdr = msg->data<protocol::ft_header>();
    hdr->type = protocol::pkt_type::FT_MSG;
    hdr->start = 1;
    hdr->end = 1;
    hdr->seq = seq;
    *msg->get_ts() = 0;
    return msg;
  }

  message_allocator *allocator;
  transport_output *to;
};

TEST_F(TransportOutputTest, Placeholder) {
    static constexpr unsigned kTestWnd = 16;
    std::vector<message*> msgs(kTestWnd);

}
