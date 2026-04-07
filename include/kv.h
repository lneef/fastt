#pragma once

#include "client.h"
#include "connection.h"
#include "sgl.h"
#include "util.h"
#include <cstddef>
#include <cstdint>

#include <vector>

struct kv_slot {
  uint16_t id;
  int64_t key;
  uint64_t ts;
};

struct kv_slot_store {
  std::vector<kv_slot> slots;
  std::deque<uint16_t> free_slots;
  kv_slot_store(uint16_t sz) : slots(sz) {
    for (auto i = 0u; i < sz; ++i) {
      free_slots.push_back(i);
      slots[i].id = i;
    }
  }

  bool empty() const { return free_slots.empty(); }

  uint16_t get() {
    auto id = free_slots.front();
    free_slots.pop_front();
    return id;
  }

  void put(uint16_t id) { free_slots.push_back(id); }
};

class kv_proxy {
public:
  kv_proxy(client_iface *ifc) : ifc(ifc), slots(128) {}

  int connect(const con_config &target, rte_ether_addr &dmac) {
    con = ifc->open(target, dmac);
    if (!con)
      return -1;
    return 0;
  }

  kv_slot &operator[](size_t i) { 
      assert(i < slots.slots.size());
      return slots.slots[i]; 
  }

  kv_slot *start() {
    if (!con->can_send())
      return nullptr;
    if (slots.empty())
      return nullptr;
    auto id = slots.get();
    return &slots.slots[id];
  }

  void complete(uint16_t id) { slots.put(id); }

  ssize_t recv(sgl &rsgl) { return con->recv(rsgl); }

  ssize_t send(sgl &ssgl) { return con->send_sgl(ssgl); }

  void acknowledge_all() { con->acknowledge(); }

  void close() { ifc->close(*con); }

  void flush() { ifc->flush(); }

private:
  client_iface *ifc;
  kv_slot_store slots;

public:
  connection *con;
};
