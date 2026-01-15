#include "connection.h"
#include "log.h"
#include "message.h"
#include "util.h"

#include <cstdint>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memcpy.h>

void connection::process_pkt(rte_mbuf *pkt) {
  auto *msg = static_cast<message*>(pkt);  
  if (!transport_impl->process_pkt(msg))
    return;
  *msg->get_con_ptr() = this;
} 

uint16_t connection::receive_message(message** msgs, uint16_t cnt){
    return transport_impl->receive_messages(msgs, cnt);
}

void connection::accept(const con_config& target){
    transport_impl->open_connection(target);
}

bool connection::send_message(message *pkt, uint16_t len) {
  pkt->set_size(len);
  FASTT_LOG_DEBUG("Sent pkt of len %u\n", len);
  return transport_impl->send_pkt(pkt, peer_con_config);
}

void connection::acknowledge_all() { transport_impl->send_acks(peer_con_config); }

void connection::open_connection(const con_config& target){
    transport_impl->open_connection(target);
}

bool connection::has_ready_message() const {
  return transport_impl->poll();
}
