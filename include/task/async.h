#pragma once
#include "connection.h"
#include "sgl.h"
#include "task.h"

inline concurrency::send_awaitable_sgl<connection> send(concurrency::scheduler &schdlr,
                                             connection &con, sgl&& msgl) {
  return concurrency::send_awaitable_sgl<connection>(schdlr, con, std::move(msgl));
}

inline concurrency::recv_awaitable_sgl<connection> recv(concurrency::scheduler &schdlr,
                                             connection &con, sgl &msgl) {
  return concurrency::recv_awaitable_sgl<connection>(schdlr, con, &msgl);
}
