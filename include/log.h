#pragma once

#include <cstdint>
#include <rte_log.h>

#define DEBUG 1
struct message;
void dump_pkt(message *msg, uint16_t len);

#ifdef DEBUG
#define FASTT_LOG_DEBUG(...)                                                   \
  printf(__VA_ARGS__)
#define FASTT_DUMP_PKT(msg, len) dump_pkt(msg, len)
#else
#define FASTT_LOG_DEBUG(...)
#define FASST_DUMP_PKT(msg, len)
#endif // DEBUG
