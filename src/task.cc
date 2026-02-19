#include "task.h"
#include <coroutine>

#include "connection.h"
namespace concurrency {

    void task::yield::await_suspend(std::coroutine_handle<promise_type> caller){
        schdlr.schedule(caller);
    }



    bool send_awaitable::await_ready() noexcept{
        return con.can_send();
    }

    void send_awaitable::await_suspend(std::coroutine_handle<task::promise_type> caller){

    }

    size_t send_awaitable::await_resume() noexcept{
        return con.send(buf, size, meta);
    }
}
