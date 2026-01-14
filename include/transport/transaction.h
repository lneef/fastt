#pragma once
#include "message.h"
#include "protocol.h"
#include "transport/retransmission_handler.h"
#include "transport/transport.h"
#include <cstdint>
#include <rte_mbuf.h>

struct transaction {
  static constexpr std::size_t kTimeout = 1;
  uint16_t tid;
  window recv_wd;
  retransmission_handler<false> rt_handler;

  void probe_timeout() {
    rt_handler.probe_retransmit([](message *msg) { (void)msg; });
  }

  void process(message *msg) {
    auto *data_pkt = reinterpret_cast<protocol::ft_header *>(msg->data());
    if (recv_wd.set(data_pkt->seq, msg))
      return;
    rte_pktmbuf_free(msg);
  }

  void prepare_completion_fragment(message *msg) { (void)msg; }
};
