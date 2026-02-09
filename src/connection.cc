#include "connection.h"
#include "debug.h"
#include "message.h"
#include "protocol.h"

#include <cassert>
#include <cstdlib>
#include <rte_branch_prediction.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memcpy.h>

void connection::process_pkt(rte_mbuf *pkt) {
  for(auto* msg = pkt; msg; ){
      auto* hdr = static_cast<message*>(pkt)->data<protocol::ft_header>();
      if(unlikely(hdr->len > RTE_ETHER_MAX_LEN)){
          assert(0 && "Jumbo Frames not supported");
          std::abort();
      }
      auto *tpkt = msg;
      msg = msg->next;
      tpkt->next = nullptr;
      tpkt->nb_segs = 1;
      tpkt->pkt_len = tpkt->data_len;
      transport_impl->process_pkt(msg_fragment(static_cast<message*>(tpkt)));
  }   
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
