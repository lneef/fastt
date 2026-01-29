#include "kv.h"
#include "transaction.h"
#include <rte_mbuf_core.h>

std::unique_ptr<transaction_proxy> kv_proxy::start_transaction(connection *con,
                                                           transaction_queue &q) {
  auto* slot = con->start_transaction();  
  if(!slot)
      return nullptr;
  auto* th = q.enqueue(slot);
  return std::make_unique<transaction_proxy>(q, con, th);
}

void kv_proxy::finish_transaction(transaction_proxy* proxy){
    proxy->con->finish_transaction(proxy->t->slot);
}
