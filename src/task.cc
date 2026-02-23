#include "connection.h"
#include "task.h"
#include <coroutine>

namespace concurrency {

void task::yield::await_suspend(std::coroutine_handle<promise_type> caller) {
  schdlr.schedule(caller);
}

bool send_awaitable::await_ready() noexcept {
  if (!con.can_send())
    return false;
  auto sent = con.send(*hdr.hdr);
  hdr.retval += sent;
  return true;
}

void send_awaitable::await_suspend(
    std::coroutine_handle<task::promise_type> caller) {
  con.coro = caller;
  auto &prms = caller.promise();
  prms.hdr = &hdr;
  prms.yt = io_yield_type::send_yield;
}

bool recv_awaitable::await_ready() noexcept {
  if (!con.can_recv())
    return false;
  auto rcvd = con.recv(*hdr.hdr);
  hdr.retval += rcvd;
  if (rcvd == hdr.retval || hdr.hdr->flags == 0) {
    return true;
  } else {
    return false;
  }
}

void recv_awaitable::await_suspend(
    std::coroutine_handle<task::promise_type> caller) {
  con.coro = caller;
  auto &prms = caller.promise();
  prms.hdr = &hdr;
  prms.yt = io_yield_type::recv_yield;
}
} // namespace concurrency
