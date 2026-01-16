#pragma once

#include "message.h"
#include "protocol.h"
#include "transport/retransmission_handler.h"
#include "util.h"
#include "window.h"

#include <cstdint>
#include <deque>
#include <generic/rte_cycles.h>
#include <rte_mbuf.h>

struct transport;

template<typename>
class session;
class client_slot;
class server_slot;

class slot{
public:
    static constexpr uint16_t kMaxOustandingMessages = 64;  
  slot(uint16_t id, transport *transport_impl)
      : msg_id(id), wnd(1), 
        default_timeout(rte_get_timer_hz() / 1e3), transport_impl(transport_impl) {
            timeout = rte_get_timer_cycles() + default_timeout;
        }
  slot() : msg_id(0), wnd(1), default_timeout(rte_get_timer_hz() / 1e3) {}

  bool poll() const;

  bool done() const;

  void retransmit();

  void send_init(uint16_t wnd);

  uint32_t ready_to_ack() const;

  bool timed_out(uint64_t now) const;

  void acknowledge() const;

  void advance();

  void accept(uint16_t wnd);

  bool receive_msg(message **msg);

  void rearm();

  uint32_t cleanup_buffered_msgs(uint32_t seq);

protected:
  enum class state { REQUEST, RESPONSE, COMPLETED, COMPLETION_PENDING };
  uint16_t msg_id;
  state cstate = state::COMPLETED;
  window<kMaxOustandingMessages> wnd;
  std::deque<message*> msg_buffer;

  union{
      session<client_slot> *client_session;
      session<server_slot> *server_session;
  };

  const uint64_t default_timeout;
  uint64_t timeout = 0;
  uint32_t next_seq = min_seq;
  uint32_t nb_segs = 0;
  uint32_t last_cleanuped_seq;
  uint32_t used_grant = 0;
  uint16_t *credits;
  bool urgent = 0;
  transport *transport_impl;
};

class client_slot : public slot, public intrusive_list<client_slot> {
public:
  client_slot(uint16_t id, transport *transport_impl)
      : slot(id, transport_impl) {}
  client_slot() : slot() {};

  bool send_message(message *msg, uint64_t out);

  void process(message *msg, protocol::ft_header *hdr);

  void cleanup();
};

class server_slot : public slot {
public:
  server_slot(uint16_t id, transport *transport_impl)
      : slot(id, transport_impl) {}
  server_slot() : slot() {}

  bool send_message(message *msg, uint64_t out);

  void process(message *msg, protocol::ft_header *hdr);

  void cleanup();
};
