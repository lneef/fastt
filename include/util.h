#pragma once
#include <arpa/inet.h>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/list_hook.hpp>
#include <boost/intrusive/options.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <rte_cycles.h>
#include <string>
#include <utility>

#include <boost/intrusive/list.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#define ensure(x)                                                              \
  do {                                                                         \
    if (!(x))                                                                  \
      std::abort();                                                            \
  } while (0);

namespace bi = boost::intrusive;
namespace bu = boost::unordered;

extern uint64_t to_us;
extern uint64_t to_ms;

/*
 * bitcast
 */
namespace cast {
template <typename To, typename From>
typename std::enable_if<sizeof(To) == sizeof(From) &&
                            std::is_trivially_copyable<From>::value &&
                            std::is_trivially_copyable<To>::value,
                        To>::type

bit_cast(const From &src) noexcept {
  To dst;
  std::memcpy(&dst, &src, sizeof(To));
  return dst;
}
}; // namespace cast
template <typename T, unsigned N> struct packet_vector {
  std::array<T, N> pkts;
  uint16_t i = 0;

  constexpr void clear() { i = 0; }

  auto begin() { return pkts.begin(); }
  auto end() { return pkts.begin() + i; }
};

void init_timing();

using list_hook =
    bi::list_member_hook<bi::link_mode<bi::link_mode_type::auto_unlink>>;

template <typename Key, typename T>
using flow_table = bu::unordered_flat_map<Key, T>;

template <typename T, list_hook T::*link = &T::link>
using intrusive_list_t = bi::list<T, bi::member_hook<T, list_hook, link>,
                                  bi::constant_time_size<false>>;

__inline uint64_t get_ticks_us() { return to_us; }

__inline uint64_t get_ticks_ms() { return to_ms; }

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

__inline constexpr std::pair<unsigned, unsigned>
get_bit_indices_64(unsigned i) {
  return {i / 64, i & 63};
}

struct flow_tuple {
  uint32_t sip, dip;
  uint16_t sport, dport;

  std::string print() const {
    return std::format("{}.{}.{}.{}:{} -> {}.{}.{}.{}:{}", sip & 0xff,
                       (sip >> 8) & 0xff, (sip >> 16) & 0xff, sip >> 24,
                       ntohs(sport), dip & 0xff, (dip >> 8) & 0xff,
                       (dip >> 16) & 0xff, dip >> 24, ntohs(dport));
  }

  friend bool operator==(const flow_tuple &lhs, const flow_tuple &rhs);
};

template <typename T> inline uint32_t calc_hash(const T &key);

template <> inline uint32_t calc_hash<flow_tuple>(const flow_tuple &tuple) {
  return jhash_3words(tuple.sip, tuple.dip, tuple.sport | (tuple.dport) << 16);
}

template <> inline uint32_t calc_hash<uint32_t>(const uint32_t &val) {
  return jhash_3words(val, 0, 0);
}

inline std::size_t hash_value(const flow_tuple &ft) { return calc_hash(ft); }

struct transport_config {
  uint32_t ip;
  struct {
    uint16_t sport, dport;
  } transport_ports;
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
