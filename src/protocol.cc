#include "protocol.h"
#include "message.h"
#include <cstdint>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>

void protocol::prepare_ft_header(message* msg, seq_t seq, seq_t ack, uint16_t wnd, bool start, bool end, uint32_t us, bool is_sack){
    auto *ft = msg->move_headroom<protocol::ft_header>();
    ft->ack = ack;
    ft->seq = seq;
    ft->wnd = wnd;
    ft->start = start;
    ft->end = end;
    ft->ts = us;
    ft->sack = is_sack;
    ft->type = protocol::pkt_type::FT_MSG;
}

void protocol::prepare_ack_pkt(message* msg, seq_t ack, uint32_t us, bool is_sack){
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header*);
    ft->ack = ack;
    ft->sack = is_sack;
    ft->wnd = 0;
    ft->ts = us;
    ft->type = protocol::pkt_type::FT_ACK;
}


void protocol::prepare_init_header(message* msg, seq_t seq, uint16_t budget){
    auto *ft = static_cast<ft_header*>(msg->data());
    ft->seq = seq;
    ft->ts = 0;
    ft->sack = 0;
    ft->wnd = budget;
    ft->start = true;
    ft->end = true;
    ft->type = protocol::pkt_type::FT_RDY_TO_RCV;
}


void protocol::prepare_ctrl_pkt(message* msg, seq_t seq, uint16_t wnd){
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header*);
    ft->seq = seq;
    ft->wnd = wnd;
    ft->type = protocol::pkt_type::FT_CRTL;
}

void protocol::prepare_init_ack_header(message* msg, seq_t seq, seq_t ack, uint16_t wnd){
    auto *ft = rte_pktmbuf_mtod(msg, protocol::ft_header*);
    ft->ack = ack;
    ft->wnd = wnd;
    ft->seq = seq;
    ft->ts = 0;
    ft->sack = 0;
    ft->start = true;
    ft->end = true;
    ft->type = protocol::pkt_type::FT_CLR_TO_SD;
}
