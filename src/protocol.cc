#include "message.h"
#include "protocol.h"
#include <cstdint>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>

void protocol::prepare_ft_header(message *msg, uint64_t seq, uint64_t ack,
                                 uint32_t msg_id, uint16_t wnd, uint64_t out, bool fin) {
  auto *ft = msg->move_headroom<protocol::ft_header>();
  ft->ack = ack;
  ft->seq = seq;
  ft->msg_id = msg_id;
  ft->wnd = wnd;
  ft->out = out;
  ft->fini = fin;
  ft->type = protocol::pkt_type::FT_MSG;
}

message *protocol::prepare_ack_header(uint64_t ack, message_allocator *pool,
                                   uint16_t wnd, uint32_t msg_id) {
  auto *msg = pool->alloc_message(sizeof(protocol::ft_header));
  if (!msg)
    return nullptr;
  auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header *);
  ft->ack = ack;
  ft->seq = 0;
  ft->wnd = wnd;
  ft->msg_id = msg_id;
  ft->fini = 0;
  ft->out = 0;
  ft->type = protocol::pkt_type::FT_ACK;
  return msg;
}

message *protocol::prepare_init_header(uint64_t seq, message_allocator *pool,
                                   uint16_t wnd) {
  auto *msg = pool->alloc_message(sizeof(protocol::ft_header));
  if (!msg)
    return nullptr;
  auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header *);
  ft->ack = 0;
  ft->seq = seq;
  ft->wnd = wnd;
  ft->msg_id = 0;
  ft->fini = 0;
  ft->out = 0;
  ft->type = protocol::pkt_type::FT_INIT;
  return msg;
}
