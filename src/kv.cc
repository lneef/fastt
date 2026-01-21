#include "kv.h"
#include "transaction.h"
#include <rte_mbuf_core.h>

std::unique_ptr<response_proxy> kv_proxy::send_request(connection *con,
                                                           message *msg,
                                                           transaction_queue &q,
                                                           uint16_t len) {
  auto* transaction = q.enqueue();
  auto * pkt = rte_pktmbuf_mtod(msg, kv_packet_base*);
  pkt->id = transaction->id;
  pkt->pt = packet_t::SINGLE;
  con->send_message(msg, len);
  return std::make_unique<response_proxy>(q, this, con, transaction);
}


message *kv_proxy::recv_completion(connection *con){
    return ifc->recv_message(con);
}
