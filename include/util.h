#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <utility>
#include <vector>

//-------------------------------------------------------------------------------
/*
 *  Taken from linux kernel
 *  https://github.com/torvalds/linux/blob/master/include/linux/jhash.h
 *  jhash.h: Jenkins hash support.
 *
 * Copyright (C) 2006. Bob Jenkins (bob_jenkins@burtleburtle.net)
 *
 * https://burtleburtle.net/bob/hash/
 *
 * These are the credits from Bob's sources:
 *
 * lookup3.c, by Bob Jenkins, May 2006, Public Domain.
 *
 * These are functions for producing 32-bit hashes for hash table lookup.
 * hashword(), hashlittle(), hashlittle2(), hashbig(), mix(), and final()
 * are externally useful functions.  Routines to test the hash are included
 * if SELF_TEST is defined.  You can use this free for any purpose.  It's in
 * the public domain.  It has no warranty.
 *
 * Copyright (C) 2009-2010 Jozsef Kadlecsik (kadlec@netfilter.org)
 *
 * I've modified Bob's hash to be useful in the Linux kernel, and
 * any bugs present are my fault.
 * Jozsef
 */

__inline constexpr uint32_t rol32(uint32_t word, unsigned int shift) {
  return (word << (shift & 31)) | (word >> ((-shift) & 31));
}

/* __jhash_final - final mixing of 3 32-bit values (a,b,c) into c */
#define __jhash_final(a, b, c)                                                 \
  {                                                                            \
    c ^= b;                                                                    \
    c -= rol32(b, 14);                                                         \
    a ^= c;                                                                    \
    a -= rol32(c, 11);                                                         \
    b ^= a;                                                                    \
    b -= rol32(a, 25);                                                         \
    c ^= b;                                                                    \
    c -= rol32(b, 16);                                                         \
    a ^= c;                                                                    \
    a -= rol32(c, 4);                                                          \
    b ^= a;                                                                    \
    b -= rol32(a, 14);                                                         \
    c ^= b;                                                                    \
    c -= rol32(b, 24);                                                         \
  }

/* jhash_3words - hash exactly 3, 2 or 1 word(s) */
static inline uint32_t jhash_3words(uint32_t a, uint32_t b, uint32_t c,
                                    uint32_t initval = 0xfffffff) {
  static constexpr uint32_t kJHashInitial = 0xdeadbeef;
  a += kJHashInitial;
  b += kJHashInitial;
  c += initval;
  __jhash_final(a, b, c);
  return c;
}
//-------------------------------------------------------------------------------

template<typename T>
void intrusive_push_front(T& sentinel, T* elem){
    elem->next = sentinel.next;
    sentinel.next->prev = elem;
    elem->prev = &sentinel;
    sentinel.next = elem;
}

template<typename T>
T* intrusive_pop_back(T& sentinel){
    auto *tail = sentinel.prev;
    sentinel.prev = tail->prev;
    tail->prev->next = &sentinel;
    return tail;
}

template<typename T>
void intrusive_remove(T* elem){
    elem->prev->next = elem->next;
    elem->next->prev = elem->prev;
}

struct flow_tuple {
  uint32_t sip, dip;
  uint16_t sport, dport; 

  friend bool operator==(const flow_tuple &lhs, const flow_tuple &rhs);
};

template<typename T>
inline uint32_t calc_hash(const T& key);

template<>
inline uint32_t calc_hash<flow_tuple>(const flow_tuple& tuple){
    return jhash_3words(tuple.sip, tuple.dip,
                        tuple.sport | (tuple.dport) << 16); 
}

template<>
inline uint32_t calc_hash<uint32_t>(const uint32_t& val){
    return jhash_3words(val, 0, 0);
}

template <typename K, typename V> struct fixed_size_hash_table {
  using hash_t = uint32_t;  
  struct entry_t {
    bool occupied;
    K key;
    V val;
    entry_t() : occupied(false) {}
  };

  std::vector<entry_t> table;
  uint32_t mask;

  V *lookup(const K &key) {
    auto i = calc_hash(key) & mask;  
    auto searched = 0u;
    for (; searched < table.size(); i = (i + 1) & mask, ++searched) {
      if (!table[i].occupied)
        break;
      if (key == table[i].key)
        return &table[i].val;
    }
    return nullptr;
  }

  template <typename ...Args>
  std::pair<V *, bool> emplace(const K& key, Args&& ...args) {
    auto i = calc_hash(key) & mask;
    auto searched = 0u;
    for (; searched < table.size(); i = (i + 1) & mask, ++searched) {
      if (!table[i].occupied) {
        table[i].occupied = true;
        table[i].key = key;
        new (&table[i].val) V(std::forward<Args>(args)...);
        return {&table[i].val, true};
      }

      if (table[i].key == key)
        return {&table[i].val, false};
    }
    return {nullptr, false};
  }

  fixed_size_hash_table(std::size_t size)
      : table(size),  mask(size - 1) {}
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
