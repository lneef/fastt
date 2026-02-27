#pragma once

#include <cstdint>

//#define DEBUG 1
struct msg_fragment;
void dump_pkt(msg_fragment *msg, uint16_t len);

#ifdef DEBUG
#define FASTT_LOG_DEBUG(...)                                                   \
  printf(__VA_ARGS__)
#define FASTT_DUMP_PKT(msg, len) //dump_pkt(msg, len)
#else
#define FASTT_LOG_DEBUG(...)
#define FASTT_DUMP_PKT(msg, len)
#endif // DEBUG

