#pragma once

#include "client.h"
#include "connection.h"
#include "message.h"
#include "slot.h"
#include "util.h"
#include <cstddef>
#include <cstdint>
#include <generic/rte_cycles.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_timer.h>

#include "kv_protocol.h"
#include <vector>

inline void create_get_request(message *msg, int64_t key, uint64_t id) {
  kv::create_kv_request(static_cast<uint8_t *>(msg->data()), id, key);
}

inline void create_scan_request(message *msg, int64_t low, uint64_t high,
                                int64_t id) {
  kv::create_kv_scan(msg->data<uint8_t>(), id, low, high);
}


struct kv_slot{
    uint16_t id;
    int64_t key;
};

struct kv_slot_store{
    std::vector<kv_slot> slots;
    std::deque<uint16_t> free_slots;
    kv_slot_store(uint16_t sz): slots(sz){
        for(auto i = 0u; i< sz; ++i){
            free_slots.push_back(i);
            slots[i].id = i;
        }
    }

    bool empty() const{
        return free_slots.empty();
    }

    uint16_t get(){
        auto id = free_slots.front();
        free_slots.pop_front();
        return id;
    }

    void put(uint16_t id){
        free_slots.push_back(id);
    }
};

class kv_proxy {
public:
  kv_proxy(client_iface *ifc) : ifc(ifc), slots(128) {}

  int connect(const con_config &con_cfg, uint16_t n, rte_ether_addr &dmac) {
    con_config cfg = con_cfg;
    for (uint16_t i = 0; i < n; ++i) {
      con = ifc->open_connection(cfg, dmac);
      if (!con)
        return -1;
      while (!ifc->probe_connection_setup_done(con))
        ;
      con->acknowledge_all(rte_get_timer_cycles());
      ifc->flush();
    }
    return 0;
  }

  kv_slot& operator[](size_t i){
      return slots.slots[i];
  }

  kv_slot *start() {
    if(!con->can_send())
        return nullptr;
    if(slots.empty())
        return nullptr;
    auto id = slots.get();
    return &slots.slots[id];
  }

  void complete(uint16_t id){
      slots.put(id);
  }

  ssize_t recv(void* buf, size_t sz){
      msg_hdr m;
      m.buf = static_cast<uint8_t*>(buf);
      m.size = sz;
      m.remaining = 0;
      return con->recv(m);
  }

  ssize_t send(void* buf, size_t sz){
      msg_hdr m;
      m.buf = static_cast<uint8_t*>(buf);
      m.size = sz;
      m.som = true;
      m.eom = true;
      return con->send(m);
  }

  void lookup(int64_t key, message *msg, uint64_t id) {
    create_get_request(msg, key, id);
  };
  void scan(int64_t low, int64_t high, message *msg, uint64_t id) {
    create_scan_request(msg, low, high, id);
  }

  void acknowledge_all() {
    con->acknowledge_all(rte_get_timer_cycles());  
  }

  template <typename F> void handle_active(F &&fun) { ifc->manager.poll(fun); }

  void flush() { ifc->flush(); }

private:
  client_iface *ifc;
  kv_slot_store slots;
public:
  connection *con;
};
