#pragma once
#include <algorithm>
#include <cstdint>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>

template <typename T> struct intrusive_list {
  T *next;
  T *prev;

  void remove() {
    prev->next = next;
    next->prev = prev;
  }

  void intrusive_push_front(T *elem) {
    elem->next = next;
    elem->prev = static_cast<T*>(this);
    next = elem;
  }

  T *intrusive_pop_back() {
    auto *tail = prev;
    prev = tail->prev;
    tail->prev->next = static_cast<T*>(this);
    return tail;
  }
};

struct con_config {
  uint32_t ip;
  uint16_t port;

  con_config(uint32_t ip, uint16_t port) : ip(ip), port(port) {}

  con_config(const con_config &other) {
    ip = other.ip;
    port = other.port;
  }
};

struct flow_tuple {
  uint32_t sip, dip;
  uint16_t sport, dport;
  template <typename H> friend H AbslHashValue(H h, const flow_tuple &c) {
    return H::combine(std::move(h), c.sip, c.sip, c.sport, c.dport);
  }

  friend bool operator==(const flow_tuple &lhs, const flow_tuple &rhs);
};
