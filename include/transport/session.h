#pragma once

#include "message.h"
#include "packet_if.h"
#include "protocol.h"
#include "slot.h"
#include "transport/transport.h"
#include "util.h"
#include <cstdint>
#include <generic/rte_cycles.h>
#include <memory>
#include <rte_mbuf.h>

template <int N, typename S> struct poll_state {
  std::array<S *, N> events;
  uint16_t filled = 0;
};

template<typename T>
class session {
public:
  static constexpr uint16_t kMaxSlot = 32;  
  using slot_t = T;
  session(std::shared_ptr<message_allocator> allocator, packet_if *pkt_if,
          const con_config &target, uint16_t sport)
      : transport_impl(std::make_unique<transport>(allocator.get(), pkt_if,
                                                   sport, target)) {
    uint16_t id = 0;
    for (auto &slot : slots)
      new (&slot) class slot(id++, transport_impl.get());
  }

  template <int N> uint16_t poll(poll_state<N, T> &state) {
    uint16_t i = 0;
    for (auto &slot : slots) {
      if (state.filled == N)
        break;
      if (slot.poll()) {
        state.events[state.filled++] = &slot;
        ++i;
      }
    }
    return i;
  }

  T *process(message *msg) {
    auto *ft_hdr = msg->data<protocol::ft_header>();
    auto id = ft_hdr->msg_id;
    if (ft_hdr->seq == 0) {
      return handle_new_request(id, msg, ft_hdr);
    } else {
      slots[id].process(msg, ft_hdr);
      return &slots[id];
    }
  }

  bool accept_session() {
    if (slots[0].done()) {
      slots[0].accept(grant);
      return true;
    }
    return false;
  }

  void open_session() {
      slots[0].send_init(grant);
  }

  void wait_session_active(){ 
      while(!slots[0].done())
          ;
  }

  void check_timeouts(){
      auto now = rte_get_timer_cycles();
      for(auto& slot: slots){
          if(slot.timed_out(now))
              slot.retransmit();
          slot.rearm();
      }
  }

  virtual ~session() = default;

protected:
  T *handle_new_request(uint32_t msg_id, message *msg,
                           protocol::ft_header *hdr) {
    if (slots[msg_id].done()) {
      slots[msg_id].cleanup();  
      new (&slots[msg_id]) slot(msg_id, transport_impl.get());
      slots[msg_id].process(msg, hdr);
      return &slots[msg_id];
    } else {
      rte_pktmbuf_free(msg);
      return nullptr;
    }
  }
  uint16_t grant = slot::kMaxOustandingMessages; 
  std::array<T, kMaxSlot> slots{};
  std::unique_ptr<transport> transport_impl;
  uint32_t peer_grant;
  friend class server_slot;
  friend class client_slot;
};

using server_session = session<server_slot>;

class client_session : public session<client_slot> {
public:  
  client_session(std::shared_ptr<message_allocator> allocator,
                 packet_if *pkt_if, const con_config &target, uint16_t sport)
      : session(allocator, pkt_if, target, sport) {
    head.next = nullptr;
    tail.prev = nullptr;
    for (auto &slot : slots)
      head.intrusive_push_front(&slot);
  }

  client_slot* handle(){ return tail.intrusive_pop_back(); }

  void close(client_slot* slt){
      if(slt->done())
          head.intrusive_push_front(slt);
  }

  client_slot *reserve_slot() {
    if (head.next == &tail)
      return nullptr;
    return tail.intrusive_pop_back();
  }

  void free_slot(client_slot *slot) { head.intrusive_push_front(slot); }

  ~client_session() override = default;

private:
  client_slot head, tail;
};
