#pragma once

#include "client.h"
#include "connection.h"
#include "msg_fragment.h"
#include "sgl.h"
#include "util.h"
#include <bits/types/struct_iovec.h>
#include <cstddef>
#include <cstdint>
#include <generic/rte_cycles.h>

#include "kv_protocol.h"
#include <vector>

inline void create_get_request(msg_fragment *msg, int64_t key, uint64_t id) {
  kv::create_kv_request(static_cast<uint8_t *>(msg->data()), id, key);
}

inline void create_scan_request(msg_fragment *msg, int64_t low, uint64_t high,
                                int64_t id) {
  kv::create_kv_scan(msg->data<uint8_t>(), id, low, high);
}

struct kv_slot {
  uint16_t id;
  int64_t key;
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

  int connect(const con_config &target, uint16_t rtid,
              rte_ether_addr &dmac) {
    con = ifc->open(target, rtid, dmac);
    if(!con)
        return -1;
    return 0;
  }

  kv_slot &operator[](size_t i) { return slots.slots[i]; }

  kv_slot *start() {
    if (!con->can_send())
      return nullptr;
    if (slots.empty())
      return nullptr;
    auto id = slots.get();
    return &slots.slots[id];
  }

  void complete(uint16_t id) { slots.put(id); }

  ssize_t recv(sgl& rsgl) {
    return con->recv(rsgl);
  }

  ssize_t send(sgl &ssgl) {
    return con->send(ssgl);
  }

  void lookup(int64_t key, msg_fragment *msg, uint64_t id) {
    create_get_request(msg, key, id);
  };
  void scan(int64_t low, int64_t high, msg_fragment *msg, uint64_t id) {
    create_scan_request(msg, low, high, id);
  }

  void acknowledge_all() { con->acknowledge_all(); }

  void close() {
      ifc->close(*con);
  }

  void flush() { ifc->flush(); }

private:
  client_iface *ifc;
  kv_slot_store slots;

public:
  connection *con;
};
