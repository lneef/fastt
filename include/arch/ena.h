#pragma once

#include <cstdint>
#include <rte_ethdev.h>
#include <vector>

#include "nic.h"
namespace ena {
static constexpr unsigned kQueueByteOffset = 9;
static constexpr unsigned kQueueByteStep = 2;

struct ena : public nic{
  unsigned best = 0;
  unsigned best_idx = 0;
  std::vector<uint64_t> ids;
  std::vector<uint64_t> values;

  void update(int port) override{
    best = UINT64_MAX;
    best_idx = 0;
    rte_eth_xstats_get_by_id(port, ids.data(), values.data(), ids.size());
    for (unsigned i = 0; i < ids.size(); ++i) {
      if (values[i] < best) {
        best = values[i];
        best_idx = i;
      }
    }
  }

  unsigned best_queue() const override { return best_idx; }

  ena(unsigned n_qpair) : nic(n_qpair), ids(n_qpair), values(n_qpair) {
    unsigned i = 0;
    for (auto &id : ids)
      id = kQueueByteOffset + i++ * kQueueByteStep;
  }
};

} // namespace ena
