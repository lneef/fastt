#pragma once

#include <cstdint>
#include <memory.h>
#include <memory>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_udp.h>

#include <absl/container/flat_hash_map.h>

#include "dev.h"
#include "log.h"
#include "message.h"
#include "packet_if.h"
#include "protocol.h"
#include "transport/session.h"
#include "util.h"

class iface;

template <typename T> class session_manager {
  static constexpr uint16_t kdefaultBurstSize = 32;
public:
  using slot_t = typename T::slot_t;
  session_manager(uint16_t port, uint16_t txq, uint16_t rxq, uint32_t sip,
                  std::shared_ptr<message_allocator> allocator)
      : allocator(allocator), dev(port, txq, rxq), scheduler(&dev),
        pkt_if(&scheduler, sip, port) {}

  void handle_pkt(message *pkt, flow_tuple &ft) {
    FASTT_LOG_DEBUG("Got new pkt from: %d, %d\n", ft.sip,
                    rte_be_to_cpu_16(ft.sport));
    auto *header = rte_pktmbuf_mtod(pkt, protocol::ft_header *);
    if (header->type == protocol::FT_INIT)
      register_request(pkt, ft);
    else {
      auto *ses = lookUp(ft);
      if (ses)
        ses->process(pkt);
      else
        rte_pktmbuf_free(pkt);
    }
  }

  void add_mac(uint32_t ip, rte_ether_addr &mac) {
    pkt_if.add_mapping(ip, mac);
  }

  T *lookUp(const flow_tuple &tuple) {
    if (cons.contains(tuple))
      return nullptr;
    return cons[tuple].get();
  }

  T *open_session(const con_config &source, const con_config &target) {
    flow_tuple ft(target.ip, source.ip, rte_cpu_to_be_16(target.port),
                  rte_cpu_to_be_16(source.port));
    FASTT_LOG_DEBUG("Opened new session to %d %d\n", ft.sip,
                    rte_be_to_cpu_16(ft.sport));
    auto [it, inserted] =
        cons.emplace(ft, std::make_unique<T>(
                             allocator, &pkt_if, target, source.port));
    if (!inserted)
      return nullptr;
    it->second->open_session();
    return it->second.get();
  }

  template <int N> uint16_t poll(poll_state<N, slot_t> &events) {
    fetch_from_device();
    uint16_t i = 0;
    for (auto it = cons.begin(), end = cons.end(); i < N && it != end; ++it) {
      i += it->second->poll(events);
      if (events.filled == N)
        return i;
    }
    return i;
  }

  void fetch_from_device() {
    dev.rx_burst([this](message *pkt) {
      flow_tuple ft;
      auto *msg = pkt_if.consume_pkt(pkt, ft);
      if (!msg)
        return;
      handle_pkt(pkt, ft);
    });
  }

  void register_request(message *pkt, flow_tuple &ft) {
    FASTT_LOG_DEBUG("Registering new request");
    session_requests.emplace_back(pkt, ft);
  }

  slot_t *accept_slot() {
    if (session_requests.empty())
      return nullptr;
    auto [pkt, ft] = session_requests.front();
    auto [con, inserted] = add_slot(ft, rte_be_to_cpu_16(ft.sport));
    auto *slot = con->process(pkt);
    if (inserted)
      con->accept_session();
    FASTT_LOG_DEBUG("Added new slot from %u %d\n", ft.sip, ft.sport);
    return slot;
  }

  std::pair<T *, bool> add_slot(const flow_tuple &tuple, uint16_t port) {
    auto [it, inserted] = cons.emplace(
        tuple, std::make_unique<T>(
                   allocator, &pkt_if,
                   con_config{tuple.dip, rte_cpu_to_be_16(tuple.dport)}, port));
    return {it->second.get(), inserted};
  }

  void flush() { scheduler.flush(); }

private:
  std::deque<std::pair<message *, flow_tuple>> session_requests;
  absl::flat_hash_map<flow_tuple, std::unique_ptr<T>> cons;
  std::shared_ptr<message_allocator> allocator;
  netdev dev;
  packet_scheduler scheduler;
  packet_if pkt_if;
};
