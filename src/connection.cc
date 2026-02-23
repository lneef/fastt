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
  switch (prms.yt) {
  case concurrency::io_yield_type::recv_yield:
    if (can_recv()) {
      auto rcvd = recv(*prms.hdr->hdr);
      prms.hdr->retval += rcvd;
      if (static_cast<size_t>(prms.hdr->retval) ==
              prms.hdr->hdr->iov->iov_len ||
          prms.hdr->hdr->flags == 0)
        op_completed = true;
    }
    break;
  case concurrency::io_yield_type::send_yield:
    if (can_send()) {
      auto retval = send(*prms.hdr->hdr);
      if (retval <= 0)
        op_completed = true;
      if (retval == prms.hdr->hdr->iov_len)
        op_completed = true;
    }
    break;
  }
  if (op_completed) {
    prms.schdlr->schedule(*coro);
    coro.reset();
  }
}
