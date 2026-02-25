#include "task.h"
#include <coroutine>
#include <sys/types.h>

namespace concurrency {

void task::yield::await_suspend(std::coroutine_handle<promise_type> caller) {
  schdlr.schedule(caller);
}
} // namespace concurrency
