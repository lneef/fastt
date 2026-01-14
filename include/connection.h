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
#include "transport/transport.h"
#include "util.h"

class iface;
class connection;

template <int N> struct poll_state {
  std::array<connection *, N> events;
};

class connection {
public:
  connection(message_allocator *allocator, packet_if *pkt_if,
             const con_config &target, uint16_t sport)
      : peer_con_config(target), allocator(allocator),
        transport_impl(std::make_unique<transport>(allocator, pkt_if, sport)) {}

  void process_pkt(rte_mbuf *pkt);
  bool send_message(message *msg, uint16_t len);
  void acknowledge_all();
  void accept(const con_config& target);
  uint16_t receive_message(message **msgs, uint16_t cnt);
  bool has_ready_message() const;
  void open_connection(const con_config& target);
  bool poll() const { return has_ready_message(); }
  bool active() { return transport_impl->active(); }

private:
  con_config peer_con_config;
  message_allocator *allocator;
  std::unique_ptr<transport> transport_impl;
};

class connection_manager {
  static constexpr uint16_t kdefaultBurstSize = 32;

public:
  connection_manager(uint16_t port, uint16_t txq, uint16_t rxq, uint32_t sip,
                     std::shared_ptr<message_allocator> allocator)
      : allocator(allocator), dev(port, txq, rxq), scheduler(&dev),
        pkt_if(&scheduler, sip, port) {}

  void handle_pkt(rte_mbuf *pkt, flow_tuple &ft) {
    FASTT_LOG_DEBUG("Got new pkt from: %d, %d\n",
            ft.sip, rte_be_to_cpu_16(ft.sport));
    auto *header = rte_pktmbuf_mtod(pkt, protocol::header_base *);
    if (header->type == protocol::FT_INIT)
      register_request(pkt, ft);
    else {
      auto *connection = lookUp(ft);
      if (connection)
        connection->process_pkt(pkt);
      else
        rte_pktmbuf_free(pkt);
    }
  }

  void add_mac(uint32_t ip, rte_ether_addr &mac) {
    pkt_if.add_mapping(ip, mac);
  }

  connection *lookUp(const flow_tuple &tuple) {
    if (cons.contains(tuple))
      return nullptr;
    return cons[tuple].get();
  }

  connection *open_connection(const con_config &source,
                              const con_config &target) {
    flow_tuple ft(target.ip, source.ip, rte_cpu_to_be_16(target.port),
                  rte_cpu_to_be_16(source.port));
    FASTT_LOG_DEBUG("Opened new connection to %d %d\n",
            ft.sip, rte_be_to_cpu_16(ft.sport));
    auto [it, inserted] =
        cons.emplace(ft, std::make_unique<connection>(allocator.get(), &pkt_if,
                                                      target, source.port));
    if (!inserted)
      return nullptr;
    it->second->open_connection(target);
    return it->second.get();
  }

  template <int N> uint16_t poll(poll_state<N> &events) {
    fetch_from_device();
    uint16_t i = 0;
    for (auto it = cons.begin(), end = cons.end(); i < N && it != end; ++it) {
      if (it->second->poll())
        events.events[i++] = it->second.get();
    }
    return i;
  }

  void fetch_from_device() {
    dev.rx_burst([this](rte_mbuf *pkt) {
      flow_tuple ft;
      auto *msg = pkt_if.consume_pkt(pkt, ft);
      if (!msg)
        return;
      handle_pkt(pkt, ft);
    });
  }

  void register_request(rte_mbuf *pkt, flow_tuple &ft) {
    FASTT_LOG_DEBUG("Registering new request");
    connection_requests.emplace_back(pkt, ft);
  }

  connection *accept_connection() {
    auto [pkt, ft] = connection_requests.front();
    auto [con, inserted] = add_connection(ft, rte_be_to_cpu_16(ft.sport));
    con->process_pkt(pkt);
    if(inserted)
        con->accept({ft.sip, ft.sport});
    return con;
  }
  std::pair<connection *, bool> add_connection(const flow_tuple &tuple, uint16_t port) {
    auto [it, inserted] = cons.emplace(
        tuple, std::make_unique<connection>(
                   allocator.get(), &pkt_if,
                   con_config{tuple.dip, rte_cpu_to_be_16(tuple.dport)}, port));
    return {it->second.get(), inserted};
  }

  void flush() { scheduler.flush(); }

private:
  std::deque<std::pair<rte_mbuf *, flow_tuple>> connection_requests;
  absl::flat_hash_map<flow_tuple, std::unique_ptr<connection>> cons;
  std::shared_ptr<message_allocator> allocator;
  netdev dev;
  packet_scheduler scheduler;
  packet_if pkt_if;
};
