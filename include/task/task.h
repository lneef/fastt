#pragma once

#include "sgl.h"
#include <bits/types/struct_iovec.h>
#include <coroutine>
#include <deque>
#include <optional>
#include <sys/types.h>

namespace concurrency {

enum class io_yield_type { recv_yield = 0, send_yield };
class scheduler;

struct task {
  struct promise_type {
    sgl *segs;
    ssize_t *retval;
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
  switch (prms.yt) {
  case concurrency::io_yield_type::recv_yield: {
    auto retval = con.recv(*prms.segs);
    if (retval == -EAGAIN)
      return;
    *prms.retval = retval;
    op_completed = true;
  } break;
  case concurrency::io_yield_type::send_yield: {
    auto retval = con.send(*prms.segs);
    if (retval == -EAGAIN)
      return;
    *prms.retval = retval <= 0 ? retval : retval + *prms.retval;
    op_completed = retval <= 0 || prms.segs->empty();
  } break;
  }
  if (op_completed) {
    prms.schdlr->schedule(*con.coro);
    con.coro.reset();
  }
}

class scheduler;

template <typename C> struct io_awaitable_sgl {
  scheduler &schdlr;
  C &con;
  ssize_t retval = 0;
  io_awaitable_sgl(scheduler &schdlr, C &con) : schdlr(schdlr), con(con) {}
};

template <typename C> struct send_awaitable_sgl : public io_awaitable_sgl<C> {
  using io_awaitable_sgl<C>::con;
  using io_awaitable_sgl<C>::retval;
  using io_awaitable_sgl<C>::schdlr;
  sgl msgl;
  send_awaitable_sgl(scheduler &schdlr, C &con, sgl &&hdr)
      : io_awaitable_sgl<C>(schdlr, con) {
    msgl = std::move(hdr);
  }

  bool await_ready() noexcept {
    auto sent = con.send(msgl);
    if (sent == -EAGAIN)
      return false;
    retval = sent;
    if (sent <= 0)
      return true;
    return msgl.empty();
  }

  void await_suspend(std::coroutine_handle<task::promise_type> caller) {
    con.coro = caller;
    auto &prms = caller.promise();
    prms.segs = &msgl;
    prms.retval = &retval;
    prms.yt = io_yield_type::send_yield;
    prms.schdlr = &schdlr;
  }

  ssize_t await_resume() noexcept { return retval; };
};

template <typename C> struct recv_awaitable_sgl : io_awaitable_sgl<C> {
  using io_awaitable_sgl<C>::con;
  using io_awaitable_sgl<C>::retval;
  using io_awaitable_sgl<C>::schdlr;
  sgl *msgl;
  recv_awaitable_sgl(scheduler &schdlr, C &con, sgl *msgl)
      : io_awaitable_sgl<C>(schdlr, con), msgl(msgl) {}

  bool await_ready() noexcept {
    auto rcvd = con.recv(*msgl);
    if (rcvd == -EAGAIN)
      return false;
    retval = rcvd;
    return true;
  }

  void await_suspend(std::coroutine_handle<task::promise_type> caller) {
    con.coro = caller;
    auto &prms = caller.promise();
    prms.segs = msgl;
    prms.retval = &retval;
    prms.yt = io_yield_type::recv_yield;
    prms.schdlr = &schdlr;
  }

  ssize_t await_resume() noexcept { return retval; };
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
