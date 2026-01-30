#include "connection.h"
#include "debug.h"
#include "message.h"

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memcpy.h>

void connection::process_pkt(rte_mbuf *pkt) {
  auto *msg = static_cast<message*>(pkt);  
  if (!transport_impl->process_pkt(msg))
    return;
} 

void connection::acknowledge_all(){
    for(auto& slot: slots)
        slot.acknowledge();
}

void connection::accept(){
    transport_impl->accept_connection();
}

void connection::open_connection(){
    transport_impl->open_connection();
}
