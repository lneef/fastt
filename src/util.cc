#include "util.h"
#include <cstdint>

uint64_t to_us, to_ms;

void init_timing(){
    to_us = rte_get_timer_hz() / 1e6;
    to_ms = rte_get_timer_hz() / 1e3;
}

bool operator==(const flow_tuple& lhs, const flow_tuple& rhs) {
  return lhs.sip == rhs.sip && lhs.dip == rhs.dip && lhs.sport == rhs.sport && lhs.dport == rhs.dport;
}
