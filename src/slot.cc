#include "connection.h"
#include "slot.h"

bool slot::send(message *msg) {
  return con->send_pkt(msg, id, true, true);
}

bool slot::can_send() { return con->capacity() > 0; }
