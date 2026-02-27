#include "connection.h"
#include "debug.h"
#include "msg_fragment.h"
#include "server.h"

#include <cassert>
#include <cstdint>
#include <netinet/in.h>
#include <rte_branch_prediction.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memcpy.h>

void connection::process_pkt(rte_mbuf *pkt) {
  transport_impl->process_pkt((static_cast<msg_fragment *>(pkt)));
}

void connection::acknowledge_all(uint64_t now) {
  transport_impl->acknowledge(now);
}

void connection::accept() { transport_impl->accept_connection(); }

void connection::open_connection() { transport_impl->open_connection(); }

void connection_manager::run(concurrency::scheduler &scheduler) {
  fetch_from_qpair();
  accept_connections([&](connection *con) {
    assert(server_parent->services.find(ntohs(con->get_flow_tuple().dport)) !=
           server_parent->services.end());
    auto service_handler =
        server_parent->services[ntohs(con->get_flow_tuple().dport)];
    scheduler.schedule(service_handler(scheduler, *con).handle);
  });
  for (auto &con : active)
    concurrency::make_progress(con);
  scheduler.run();
  acknowledge_all_and_reap();
}
