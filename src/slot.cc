#include "message.h"
#include "protocol.h"
#include "transport/slot.h"
#include "transport/transport.h"
#include "transport/session.h"
#include <cstdint>
#include <generic/rte_cycles.h>

bool slot::poll() const { return !msg_buffer.empty(); }

uint32_t slot::ready_to_ack() const { return wnd.get_last_acked_packet(); }

bool slot::timed_out(uint64_t now) const {
  return now > timeout && (cstate != state::COMPLETED);
}

void slot::retransmit(){
    for(auto *msg : msg_buffer)
        transport_impl->retransmit(msg);
}

bool slot::done() const {
  return cstate == state::COMPLETED || cstate == state::COMPLETION_PENDING;
}

void slot::acknowledge() const {
  transport_impl->send_ack(wnd.get_last_acked_packet(), msg_id, used_grant);
}

void slot::advance() {
  nb_segs += wnd.advance([&](message *msg) {
          msg_buffer.push_back(msg);
  });
}

bool slot::receive_msg(message **msg) {
  *msg = msg_buffer.front();
  return true;
}

uint32_t slot::cleanup_buffered_msgs(uint32_t seq) {
  if (seq <= last_cleanuped_seq)
    return 0;
  uint32_t cseq = seq - last_cleanuped_seq;
  uint32_t cleaned = 0;
  while (!msg_buffer.empty() && cleaned++ < cseq) {
    auto *msg = msg_buffer.front();
    msg_buffer.pop_front();
    rte_pktmbuf_free(msg);
  }
  last_cleanuped_seq = seq;
  return cseq;
}

void slot::send_init(uint16_t wnd) {
  auto *init = transport_impl->prepare_init(next_seq++, wnd);
  transport_impl->send_pkt(init);
}

void slot::accept(uint16_t wnd) {
  transport_impl->send_ack(msg_id, next_seq++, wnd);
}

void slot::rearm() { timeout = rte_get_tsc_cycles() + default_timeout; }

bool client_slot::send_message(message *msg, uint64_t out) {
  if(client_session->peer_grant == 0)
      return false;
  client_session->peer_grant--;
  msg_buffer.push_back(msg);
  protocol::prepare_ft_header(msg, next_seq++, 0, msg_id, used_grant, out,
                              out == 0);
  transport_impl->send_pkt(msg);
  if (out == 0)
    cstate = state::RESPONSE;
  return true;
}

void client_slot::process(message *msg, protocol::ft_header *hdr) {
  if (wnd.set(hdr->seq, msg))
    ++used_grant;
  if (hdr->seq < wnd.get_last_acked_packet()) {
    transport_impl->send_ack(wnd.get_last_acked_packet(), msg_id, used_grant);
    nb_segs = 0;
    rte_pktmbuf_free(msg);
    return;
  }
  if (hdr->ack)
    client_session->peer_grant += cleanup_buffered_msgs(hdr->ack);
  if (hdr->fini)
    cstate = state::COMPLETED;
  urgent = hdr->urgent;
  msg->shrink_headroom(sizeof(*hdr));
}

void client_slot::cleanup(){
    if(cstate == state::COMPLETED)
        client_session->peer_grant += cleanup_buffered_msgs(next_seq - 1);
}

bool server_slot::send_message(message *msg, uint64_t out) {
  if (server_session->peer_grant == 0)
    return false;
  server_session->peer_grant--;
  msg_buffer.push_back(msg);
  protocol::prepare_ft_header(msg, next_seq++, wnd.get_last_acked_packet(), msg_id, used_grant, out,
                              out == 0);
  transport_impl->send_pkt(msg);
  if (out == 0)
    cstate = state::COMPLETED;
  return true;
}

void server_slot::process(message *msg, protocol::ft_header *hdr) {
  if (wnd.set(hdr->seq, msg))
    ++used_grant;
  if (hdr->seq < wnd.get_last_acked_packet()) {
    transport_impl->send_ack(wnd.get_last_acked_packet(), msg_id, used_grant);
    nb_segs = 0;
    rte_pktmbuf_free(msg);
    return;
  }
  if (hdr->ack)
    server_session->peer_grant += cleanup_buffered_msgs(hdr->ack); 
  if (hdr->fini)
    cstate = state::RESPONSE;

  urgent = hdr->urgent;
  msg->shrink_headroom(sizeof(*hdr));
}

void server_slot::cleanup(){
    if(cstate == state::COMPLETED)
        server_session->peer_grant += cleanup_buffered_msgs(next_seq - 1);
}
