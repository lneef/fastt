#include "connection.h"
#include "debug.h"
#include "message.h"
#include "task.h"

#include <cassert>
#include <cstdint>
#include <optional>
#include <rte_branch_prediction.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memcpy.h>

void connection::process_pkt(rte_mbuf *pkt) {
  transport_impl->process_pkt((static_cast<message *>(pkt)));
}

void connection::acknowledge_all(uint64_t now) {
  transport_impl->acknowledge(now);
}

void connection::accept() { transport_impl->accept_connection(); }

void connection::open_connection() { transport_impl->open_connection(); }

void connection::make_progress() {
  if (coro == std::nullopt)
    return;
  auto &prms = coro->promise();
  bool op_completed = false;
  auto &mwrapper = *prms.hdr;
  switch (prms.yt) {
  case concurrency::io_yield_type::recv_yield:
    if (can_recv()) {
      auto rcvd = recv(mwrapper.buf, mwrapper.len, *mwrapper.remaining);
      if (rcvd == -EAGAIN)
        return;
      mwrapper.retval = rcvd;
      op_completed = rcvd <= 0 || *mwrapper.remaining == 0;
    }
    break;
  case concurrency::io_yield_type::send_yield:
    if (can_send()) {
      auto retval = send(*prms.hdr->hdr);
      if (retval == -EAGAIN)
        mwrapper.retval = retval > 0 ? retval + mwrapper.retval : retval;
      op_completed = retval <= 0 ||
                     mwrapper.retval == static_cast<ssize_t>(mwrapper.hdr->len);
    }
    break;
  }
  if (op_completed) {
    prms.schdlr->schedule(*coro);
    coro.reset();
  }
}

concurrency::send_awaitable connection::send(concurrency::scheduler &schdlr,
                                             msg_hdr &hdr) {
  return concurrency::send_awaitable(schdlr, *this, hdr);
}

concurrency::recv_awaitable connection::recv(concurrency::scheduler &schdlr,
                                             void *buf, size_t len,
                                             size_t &remaining) {
  return concurrency::recv_awaitable(schdlr, *this, buf, len, remaining);
}
