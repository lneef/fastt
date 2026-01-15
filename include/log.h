#pragma once
#include "message.h"

#include <cstdint>
#include <rte_log.h>

#define DEBUG

inline void dump_pkt(message *msg, uint16_t len);

#ifdef DEBUG
#define FASTT_LOG_DEBUG(...)                                                   \
  printf(__VA_ARGS__)
#define FASTT_DUMP_PKT(msg, len) dump_pkt(msg, len)
#else
#define FASTT_LOG_DEBUG(...)
#define FASST_DUMP_PKT(msg, len)
#endif // DEBUG

inline void dump_pkt(message *msg, uint16_t len) {
  static constexpr size_t bytes_per_line = 16;
  auto *data = static_cast<char *>(msg->data());
  for (size_t i = 0; i < len; i += bytes_per_line) {
    printf("%04zx  ", i);
    for (size_t j = 0; j < bytes_per_line; ++j) {
      if (i + j < len)
        printf("%02x ", data[i + j]);
      else
        printf("   ");
    }
    printf("\n");
  }
}
