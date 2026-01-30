#include "kv.h"

#include <rte_mbuf_core.h>
#include <rte_timer.h>

transaction_slot *kv_proxy::start_transaction(connection *con) {
  return con->start_transaction();
}

void kv_proxy::finish_transaction(transaction_slot* proxy){
    con->finish_transaction(proxy);
}
