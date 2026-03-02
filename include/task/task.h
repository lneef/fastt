#pragma once

#include "msg_fragment.h"
#include <bits/types/struct_iovec.h>
#include <coroutine>
#include <deque>
#include <optional>
#include <sys/types.h>

namespace concurrency {

enum class io_yield_type { recv_yield = 0, send_yield };
class scheduler;

struct msg_hdr_wrapper {
  union {
    struct {
      msg_hdr *hdr;
    };
    struct {
      void *buf;
      size_t len;
      size_t *remaining;
    };
  };
  ssize_t retval = 0;
};

struct task {
  struct promise_type {
    msg_hdr_wrapper *hdr;
    io_yield_type yt;
    scheduler *schdlr;

    task get_return_object() {
      return task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    std::suspend_always final_suspend() const noexcept { return {}; }

    void return_void() {}
    void unhandled_exception() {}
  };
  struct yield {
    scheduler &schdlr;

    yield(scheduler &schdlr) : schdlr(schdlr) {}

    constexpr bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<promise_type> caller);

    constexpr void await_resume() const noexcept {}
  };

  task(std::coroutine_handle<promise_type> handle) : handle(handle) {}

  std::coroutine_handle<promise_type> handle;
};

template <typename C> void make_progress(C &con) {
  con.perform_recovery();  
  if (con.coro == std::nullopt)
    return;
  auto &prms = con.coro->promise();
  bool op_completed = false;
  auto &mwrapper = *prms.hdr;
  switch (prms.yt) {
  case concurrency::io_yield_type::recv_yield: {
    auto rcvd = con.recv(mwrapper.buf, mwrapper.len, *mwrapper.remaining);
    if (rcvd == -EAGAIN)
      return;
    mwrapper.retval += rcvd > 0 ? rcvd : 0;
    op_completed = rcvd <= 0 || *mwrapper.remaining == 0;
  } break;
  case concurrency::io_yield_type::send_yield: {
    auto retval = con.send(*prms.hdr->hdr);
    if (retval == -EAGAIN)
      return;
    mwrapper.retval = retval > 0 ? retval + mwrapper.retval : retval;
    op_completed = retval <= 0 ||
                   mwrapper.retval == static_cast<ssize_t>(mwrapper.hdr->len);
  } break;
  }
  if (op_completed) {
    prms.schdlr->schedule(*con.coro);
    con.coro.reset();
  }
}

class scheduler;

template <typename C> struct io_awaitable {
  scheduler &schdlr;
  C &con;
  msg_hdr_wrapper mhdr;
  io_awaitable(scheduler &schdlr, C &con) : schdlr(schdlr), con(con), mhdr() {}
};

template <typename C> struct send_awaitable : public io_awaitable<C> {
  using io_awaitable<C>::con;
  using io_awaitable<C>::mhdr;
  using io_awaitable<C>::schdlr;
  send_awaitable(scheduler &schdlr, C &con, msg_hdr &hdr)
      : io_awaitable<C>(schdlr, con) {
    mhdr.hdr = &hdr;
  }

  bool await_ready() noexcept {
    auto sent = con.send(*mhdr.hdr);
    if (sent == -EAGAIN)
      return false;
    if (sent <= 0)
      return true;
    mhdr.retval = sent;
    return mhdr.retval == static_cast<ssize_t>(mhdr.hdr->len);
  }

  void await_suspend(std::coroutine_handle<task::promise_type> caller) {
    con.coro = caller;
    auto &prms = caller.promise();
    prms.hdr = &mhdr;
    prms.yt = io_yield_type::send_yield;
    prms.schdlr = &schdlr;
  }

  ssize_t await_resume() noexcept { return mhdr.retval; };
};

template <typename C> struct recv_awaitable : io_awaitable<C> {
  using io_awaitable<C>::con;
  using io_awaitable<C>::mhdr;
  using io_awaitable<C>::schdlr;
  recv_awaitable(scheduler &schdlr, C &con, void *buf, size_t len,
                 size_t &remaining)
      : io_awaitable<C>(schdlr, con) {
    mhdr.buf = buf;
    mhdr.remaining = &remaining;
    mhdr.len = len;
  }

  bool await_ready() noexcept {
    auto rcvd = con.recv(mhdr.buf, mhdr.len, *mhdr.remaining);
    if (rcvd == -EAGAIN)
      return false;
    if (rcvd <= 0)
      return true;
    mhdr.retval = rcvd;
    return *mhdr.remaining == 0;
  }

  void await_suspend(std::coroutine_handle<task::promise_type> caller) {
    con.coro = caller;
    auto &prms = caller.promise();
    prms.hdr = &mhdr;
    prms.yt = io_yield_type::recv_yield;
    prms.schdlr = &schdlr;
  }

  ssize_t await_resume() noexcept { return mhdr.retval; };
};

class scheduler {
  using task_handle = std::coroutine_handle<task::promise_type>;

public:
  scheduler() = default;

  void schedule(task_handle handle) { tasks.push_back(handle); }

  void run() {
    run([]() { return false; });
  }

  template <typename F> void run(F &&cb) {
    auto task_num = tasks.size();
    for (auto i = 0u; i < task_num; ++i) {
      auto t = tasks.front();
      tasks.pop_front();
      t.resume();
      if (t.done())
        t.destroy();

      if (cb())
        return;
    }
  }

private:
  std::deque<task_handle> tasks;
};

using coro_handle = std::coroutine_handle<task::promise_type>;

} // namespace concurrency
