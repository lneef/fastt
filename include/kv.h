#pragma once

#include "client.h"
#include "connection.h"
#include "message.h"
#include "slot.h"
#include "util.h"
#include <cstdint>
#include <generic/rte_cycles.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_timer.h>

#include "kv_protocol.h"

inline void create_get_request(message *msg, int64_t key, int64_t id) {
  kv::create_kv_request(static_cast<uint8_t *>(msg->data()), id, key);
}

inline void create_scan_request(message *msg, int64_t low, int64_t high,
                                int64_t id) {
  kv::create_kv_scan(msg->data<uint8_t>(), id, low, high);
}

class kv_proxy {
public:
  kv_proxy(client_iface *ifc, uint16_t n) : ifc(ifc), slots(n) {
    for (uint16_t i = 0; i < n; ++i)
      free_slots.push_back(i);
  }

  int connect(const con_config &con, uint16_t n, rte_ether_addr &dmac) {
    con_config cfg = con;
    cons.reserve(n);
    mask = n - 1;
    for (uint16_t i = 0; i < n; ++i) {
      auto *con = ifc->open_connection(cfg, dmac);
      if (!con)
        return -1;
      while (!ifc->probe_connection_setup_done(con))
        ;
      con->acknowledge_all();

      cfg.port = cfg.port + 1;
      cons.emplace_back(con);
    }
    return 0;
  }

  slot *start() {
    if (free_slots.empty())
      return nullptr;
    auto *con = cons[i];
    auto slot_num = free_slots.front();
    free_slots.pop_front();
    i = (i + 1) & mask;
    new (&slots[slot_num]) slot{slot_num, con};
    return &slots[slot_num];
  }

  void lookup(int64_t key, message *msg, int64_t id) {
    create_get_request(msg, key, id);
  };
  void scan(int64_t low, int64_t high, message *msg, int64_t id) {
    create_scan_request(msg, low, high, id);
  }

  void acknowledge_all(){
      for(auto* c : cons)
          c->acknowledge_all();
  }

  void finish(uint16_t id) { free_slots.push_back(id); }

  template <typename F> void poll_tx_completion(F &&cb) {
    ifc->manager.poll(cb);
  }

  void flush() { ifc->flush(); }

private:
  std::deque<uint16_t> free_slots;
  client_iface *ifc;
  std::vector<connection *> cons;
  std::vector<slot> slots;
  uint16_t i = 0;
  uint16_t mask = 0;
};
