#include "task.h"
#include <coroutine>


namespace concurrency {
    void task::yield::await_suspend(std::coroutine_handle<promise_type> caller){
        schdlr.schedule(caller);
    }
}
