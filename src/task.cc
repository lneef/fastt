#include "connection.h"
#include "task.h"
#include <cerrno>
#include <coroutine>
#include <sys/types.h>

namespace concurrency {

void task::yield::await_suspend(std::coroutine_handle<promise_type> caller) {
  schdlr.schedule(caller);
}

bool send_awaitable::await_ready() noexcept {
  if (!con.can_send())
    return false;
  auto sent = con.send(*mhdr.hdr);
  mhdr.retval = sent;
  if(sent == -EAGAIN)
      return false;
  if(sent <= 0)
      return true;
  return mhdr.retval == static_cast<ssize_t>(mhdr.hdr->len);
}

void send_awaitable::await_suspend(
    std::coroutine_handle<task::promise_type> caller) {
  con.coro = caller;
  auto &prms = caller.promise();
  prms.hdr = &mhdr;
  prms.yt = io_yield_type::send_yield;
}

bool recv_awaitable::await_ready() noexcept {
  if (!con.can_recv())
    return false;
  auto rcvd = con.recv(mhdr.buf, mhdr.len, *mhdr.remaining);
  mhdr.retval = rcvd;
  if(rcvd == -EAGAIN)
      return false;
  if(rcvd <= 0)
      return true;
  return *mhdr.remaining == 0;
}

void recv_awaitable::await_suspend(
    std::coroutine_handle<task::promise_type> caller) {
  con.coro = caller;
  auto &prms = caller.promise();
  prms.hdr = &mhdr;
  prms.yt = io_yield_type::recv_yield;
}
} // namespace concurrency
