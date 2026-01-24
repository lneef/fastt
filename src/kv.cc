#include "kv.h"
#include "transaction.h"
#include <rte_mbuf_core.h>

std::unique_ptr<transaction_proxy> kv_proxy::send_request(connection *con,
                                                           message *msg,
                                                           transaction_queue &q) {
  auto* slot = con->start_transaction();  
  if(!slot)
      return nullptr;
  auto* th = q.enqueue(slot);
  auto * pkt = rte_pktmbuf_mtod(msg, kv_packet_base*);
  pkt->pt = packet_t::SINGLE;
  slot->tx_if.send(msg, true);
  return std::make_unique<transaction_proxy>(q, con, th);
}
