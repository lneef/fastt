#include "protocol.h"
#include "message.h"
#include <cstdint>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>

void protocol::prepare_ft_header(message* msg, uint64_t seq, uint64_t ack, uint64_t msg_id, uint16_t wnd, bool fini, uint32_t us){
    auto *ft = msg->move_headroom<protocol::ft_header>();
    ft->ack = ack;
    ft->seq = seq;
    ft->msg_id = msg_id;
    ft->wnd = wnd;
    ft->fini = fini;
    ft->ts = us;
    ft->sack = 0;
    ft->type = protocol::pkt_type::FT_MSG;
}

void protocol::prepare_ack_pkt(message* msg, uint64_t ack, uint16_t wnd, uint32_t us, bool is_sack){
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header*);
    ft->ack = ack;
    ft->sack = is_sack;
    ft->seq = 0;
    ft->wnd = wnd;
    ft->ts = us;
    ft->type = protocol::pkt_type::FT_ACK;
}


void protocol::prepare_init_header(message* msg, uint64_t seq){
    auto *ft = static_cast<ft_header*>(msg->data());
    ft->seq = seq;
    ft->msg_id = 0;
    ft->ts = 0;
    ft->sack = 0;
    ft->type = protocol::pkt_type::FT_INIT;
}


void protocol::prepare_init_ack_header(message* msg, uint64_t seq, uint64_t ack, uint16_t wnd){
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header*);
    ft->ack = ack;
    ft->wnd = wnd;
    ft->seq = seq;
    ft->ts = 0;
    ft->sack = 0;
    ft->type = protocol::pkt_type::FT_INIT_ACK;
}
