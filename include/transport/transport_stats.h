#pragma once
#include <cstdint>
#include "seq.h"

struct transport_statistics {
  uint64_t retransmitted;
  seq_t acked;
  uint64_t sent, retransmissions;
  double rtt;
  transport_statistics(uint64_t retransmitted, seq_t acked, uint64_t sent,
                       uint64_t retransmissions, uint64_t rtt_est)
      : retransmitted(retransmitted), acked(acked), sent(sent),
        retransmissions(retransmissions) {
    rtt = static_cast<double>(rtt_est);
  }

  transport_statistics()
      : retransmitted(), acked(), sent(), retransmissions(), rtt() {}
};
