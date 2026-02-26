#pragma once
#include "connection.h"
#include "task.h"

concurrency::send_awaitable<connection> send(concurrency::scheduler &schdlr,
                                             connection &con, msg_hdr &hdr) {
  return concurrency::send_awaitable<connection>(schdlr, con, hdr);
}

concurrency::recv_awaitable<connection> recv(concurrency::scheduler &schdlr,
                                             connection &con, void *buf,
                                             size_t len, size_t &remaining) {
  return concurrency::recv_awaitable<connection>(schdlr, con, buf, len,
                                                 remaining);
}
