#include "connection.h"
#include "debug.h"
#include "message.h"

#include <cassert>
#include <cstdint>
#include <rte_branch_prediction.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memcpy.h>

void connection::process_pkt(rte_mbuf *pkt) {
    transport_impl->process_pkt((static_cast<message*>(pkt)));   
} 

void connection::acknowledge_all(uint64_t now){
    transport_impl->acknowledge(now);
}

void connection::accept(){
    transport_impl->accept_connection();
}

void connection::open_connection(){
    transport_impl->open_connection();
}
