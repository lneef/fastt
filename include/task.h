#pragma once

#include "message.h"
#include "transport/msg_fragment.h"
#include <coroutine>
#include <deque>

namespace concurrency {

class connection;
class scheduler;
struct task {
  struct promise_type {
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

struct send_awaitable {
  scheduler &schdlr;
  connection &con;
  message *msg;
  msg_meta meta;

  send_awaitable(scheduler &schdlr, connection &con, message *msg, msg_meta& meta)
      : schdlr(schdlr), con(con), msg(msg), meta(meta) {}

  bool await_ready() noexcept;

  void await_suspend(std::coroutine_handle<task::promise_type> caller);

  bool await_resume() noexcept;
};

struct recv_awaitable {
  scheduler &schdlr;
  connection *con;

  recv_awaitable(scheduler &schdlr, connection *con)
      : schdlr(schdlr), con(con) {}

  bool await_ready() noexcept { return false; }

  void await_suspend(std::coroutine_handle<task::promise_type> caller);

  message_buffer await_resume() noexcept;
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
