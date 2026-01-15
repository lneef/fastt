#include "protocol.h"
#include "message.h"
#include <cstdint>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>

void protocol::prepare_ft_header(message* msg, uint64_t seq, uint64_t ack, uint64_t msg_id, uint16_t wnd){
    auto *ft = msg->move_headroom<protocol::ft_header>();
    ft->ack = ack;
    ft->seq = seq;
    ft->msg_id = msg_id;
    ft->wnd = wnd;
    ft->type = protocol::pkt_type::FT_MSG;
}

message* protocol::prepare_ack_pkt(uint64_t ack, message_allocator *pool, uint16_t wnd){
    auto* msg = pool->alloc_message(sizeof(protocol::ft_header));
    if(!msg)
        return nullptr;
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header*);
    ft->ack = ack;
    ft->seq = 0;
    ft->wnd = wnd;
    ft->type = protocol::pkt_type::FT_ACK;
    return msg;
}


message* protocol::prepare_init_header(message_allocator* allocator, uint64_t seq){
    auto* msg = allocator->alloc_message(sizeof(protocol::ft_header));
    if(!msg)
        return nullptr;
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header*);
    ft->seq = seq;
    ft->msg_id = 0;
    ft->type = protocol::pkt_type::FT_INIT;
    return msg;
}


message* protocol::prepare_init_ack_header(message_allocator* pool, uint64_t ack, uint16_t wnd){
    auto* msg = pool->alloc_message(sizeof(protocol::ft_header));
    if(!msg)
        return nullptr;
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header*);
    ft->ack = ack;
    ft->wnd = wnd;
    ft->type = protocol::pkt_type::FT_INIT_ACK;
    return msg;
}
