#include "connection.h"
#include "debug.h"
#include "message.h"
#include "server.h"
#include "task.h"

#include <cassert>
#include <cstdint>
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

concurrency::send_awaitable<connection> connection::send(concurrency::scheduler &schdlr,
                                             msg_hdr &hdr) {
  return concurrency::send_awaitable<connection>(schdlr, *this, hdr);
}

concurrency::recv_awaitable<connection> connection::recv(concurrency::scheduler &schdlr,
                                             void *buf, size_t len,
                                             size_t &remaining) {
  return concurrency::recv_awaitable<connection>(schdlr, *this, buf, len, remaining);
}

void connection_manager::run(concurrency::scheduler &scheduler) {
  fetch_from_qpair();
  accept_connections([&](connection *con) {
    auto service_handler =
        server_parent->services[ntohs(con->get_flow_tuple().dport)];
    scheduler.schedule(service_handler(scheduler, *con).handle);
  });
  for (auto &con : active)
    concurrency::make_progress(con);
  scheduler.run();
  acknowledge_all_and_reap();
}
