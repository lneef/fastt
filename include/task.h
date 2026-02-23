#pragma once

#include "message.h"
#include <coroutine>
#include <deque>
#include <sys/types.h>


class connection;

namespace concurrency {
class scheduler;

struct msg_hdr_wrapper{
    msg_hdr *hdr;
    ssize_t retval = 0; 
};

enum class io_yield_type { recv_yield = 0, send_yield };
struct task {
  struct promise_type {
        msg_hdr_wrapper* hdr;  
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

using coro_handle = std::coroutine_handle<task::promise_type>;

struct io_awaitable {
  scheduler &schdlr;
  connection &con;
  msg_hdr_wrapper hdr;
  io_awaitable(scheduler &schdlr, connection &con, msg_hdr &hdr)
      : schdlr(schdlr), con(con), hdr(&hdr) {}
};

struct send_awaitable : public io_awaitable {  
  send_awaitable(scheduler &schdlr, connection &con, msg_hdr& hdr)
      : io_awaitable(schdlr, con, hdr) {}

  bool await_ready() noexcept;

  void await_suspend(std::coroutine_handle<task::promise_type> caller);

  ssize_t await_resume() noexcept{
      return hdr.retval;
  };
};

struct recv_awaitable :  io_awaitable{

  recv_awaitable(scheduler &schdlr, connection &con, msg_hdr &hdr)
      : io_awaitable(schdlr, con, hdr) {}

  bool await_ready() noexcept;

  void await_suspend(std::coroutine_handle<task::promise_type> caller);

  ssize_t await_resume() noexcept{
      return hdr.retval;
  };
};

class scheduler {
  using task_handle = std::coroutine_handle<task::promise_type>;

public:
  scheduler() = default;

  void schedule(task_handle handle) { tasks.push_back(std::move(handle)); }

  void run() {
    run([]() { return false; });
  }

  template <typename F> void run(F &&cb) {
    auto task_num = tasks.size();
    for (auto i = 0u; i < task_num; ++i) {
      auto t = tasks.front();
      t.resume();
      if (t.done())
        t.resume();
      else {
        tasks.pop_front();
        tasks.push_back(t);
      }

      if (cb())
        return;
    }
  }

private:
  std::deque<task_handle> tasks;
};
} // namespace concurrency
