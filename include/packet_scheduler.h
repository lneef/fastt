#pragma once
#include "dev.h"
#include <cstdint>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>
#include <vector>

class packet_scheduler {
public:
  static constexpr uint16_t kDefaultOutBurstSize = 32;  
  bool add_pkt(rte_mbuf *pkt);
  uint16_t flush();
  packet_scheduler(netdev *dev): dev(dev), buffer(kDefaultOutBurstSize), ptr(0) {}

private:
  uint16_t do_send();
  netdev *dev;
  std::vector<rte_mbuf *> buffer;
  std::size_t ptr;
};
