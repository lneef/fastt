#include "connection.h"
#include "debug.h"
#include "message.h"
#include "protocol.h"

#include <cassert>
#include <cstdint>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memcpy.h>

void connection::process_pkt(rte_mbuf *pkt) {
  assert(pkt->pkt_len == pkt->data_len);  
  uint16_t pkts = 0;
  auto *msg = static_cast<message*>(pkt);  
  for(auto off = 0u; off < msg->pkt_len;){
      auto* hdr = rte_pktmbuf_mtod_offset(msg, protocol::ft_header*, off);
      off += hdr->len;
      msg->inc_refcnt();
      transport_impl->process_pkt(msg_fragment(static_cast<message*>(pkt), off));
      ++pkts;
  }
  assert(rte_mbuf_refcnt_read(pkt) == pkts);
  rte_pktmbuf_free(msg);
} 

void connection::acknowledge_all(){
    transport_impl->acknowledge();
}

void connection::accept(){
    transport_impl->accept_connection();
}

void connection::open_connection(){
    transport_impl->open_connection();
}
