#include "log.h"
#include "message.h"

void dump_pkt(message *msg, uint16_t len) {
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
