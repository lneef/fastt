#include "debug.h"
#include <cstdint>
#include <cstdio>
#include <rte_mbuf_core.h>

void dump_pkt(rte_mbuf *msg, uint16_t len) {
  static constexpr size_t bytes_per_line = 16;
  auto *data = rte_pktmbuf_mtod(msg, uint8_t*);
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
