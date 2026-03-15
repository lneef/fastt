#pragma once

#include <cstdint>
#include <rte_mbuf_core.h>

#define DEBUG 1
void dump_pkt(rte_mbuf *msg, uint16_t len);

#ifdef DEBUG
#define FASTT_LOG_DEBUG(...)                                                   \
  printf(__VA_ARGS__)
#define FASTT_DUMP_PKT(msg, len) dump_pkt(msg, len)
#else
#define FASTT_LOG_DEBUG(...)
#define FASTT_DUMP_PKT(msg, len)
#endif // DEBUG

