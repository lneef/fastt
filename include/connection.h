#pragma once

#include <cstdint>
#include <deque>
#include <generic/rte_cycles.h>
#include <memory.h>
#include <memory>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_udp.h>

#include "debug.h"
#include "dev.h"
#include "message.h"
#include "packet_if.h"
#include "protocol.h"
#include "timer.h"
#include "transport/slot.h"
#include "transport/transport.h"
#include "util.h"

class iface;
class connection_manager;

class connection {
  static constexpr uint16_t kMaxTransactionPerConnection = 32;

public:
  connection(message_allocator *allocator, packet_if *pkt_if,
             const con_config &target, uint16_t sport,
             connection_manager *manager, bool is_client)
      : allocator(allocator),
        transport_impl(std::make_unique<slotted_transport>(allocator, pkt_if,
                                                           sport, target)),
        manager(manager) {
    slots.reserve(kMaxTransactionPerConnection);
    for (uint16_t i = 0; i < kMaxTransactionPerConnection; ++i) {
      slots.emplace_back(i, transport_impl.get(), is_client);
      if (is_client)
        free_slots.push_back(i);
    }
  }
  void handle_incoming_server(message *msg) {
    auto slot = transport_impl->process_pkt(msg);
    if (!slot)
      return;
    auto *hdr = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    slots[slot.value()].update_execution_state(inprogress);
    slots[slot.value()].handle_incoming_server(hdr->fini);
  }

  void handle_incoming_client(message *msg) {
    auto slot = transport_impl->process_pkt(msg);
    if (!slot)
      return;
    auto *hdr = rte_pktmbuf_mtod(msg, protocol::ft_header *);
    slots[slot.value()].handle_incoming_client(hdr->fini);
  }

  void acknowledge_open_connection(){ slots[0].acknowledge(); }
  void accept();
  uint16_t receive_message(message **msgs, uint16_t cnt);
  void open_connection();

  statistics get_transport_stats() const { return transport_impl->get_stats(); }

  bool active() { return transport_impl->active(); }

  intrusive_list_t<transaction_slot> &get_inprogress() { return inprogress; }

  transaction_slot *start_transaction() {
    if (free_slots.empty())
      return nullptr;
    auto slot_id = free_slots.front();
    free_slots.pop_front();
    slots[slot_id].update_execution();
    return &slots[slot_id];
  }

  void finish_transaction(transaction_slot *slot) {
    slot->acknowledge();  
    free_slots.push_front(slot->tid);
  }

  connection_manager *get_manager() { return manager; }

private:
  friend class connection_manager;
  message_allocator *allocator;
  std::unique_ptr<slotted_transport> transport_impl;
  std::vector<transaction_slot> slots;
  intrusive_list_t<transaction_slot, &transaction_slot::link> inprogress;
  std::deque<uint16_t> free_slots;
  connection_manager *manager;

public:
  list_hook link;
};

class connection_manager {
  static constexpr uint16_t kdefaultBurstSize = 32;
  static constexpr uint16_t kdefaultFlowTableSize = 512;

public:
  connection_manager(bool is_client, uint16_t port, uint16_t txq, uint16_t rxq,
                     uint32_t sip, std::shared_ptr<message_allocator> allocator,
                     uint16_t lcore_id)
      : flows(kdefaultFlowTableSize), allocator(allocator), dev(port, txq, rxq),
        scheduler(&dev), pkt_if(&scheduler, sip, port), active(),
        is_client(is_client), flush_timeout(get_ticks_us()),
        flush_timer(timertype::PERIODICAL) {
    flush_timer.reset(flush_timeout, flush_cb, lcore_id, this);
  }

  void handle_pkt(message *pkt, flow_tuple &ft) {
    FASTT_LOG_DEBUG("Got new pkt from: %d, %d\n", ft.sip,
                    rte_be_to_cpu_16(ft.sport));
    auto *header = rte_pktmbuf_mtod(pkt, protocol::ft_header *);
    if (header->type == protocol::FT_INIT)
      register_request(pkt, ft);
    else {
      auto *connection = flows.lookup(ft);
      if (connection)
        if (is_client)
          (*connection)->handle_incoming_client(pkt);
        else
          (*connection)->handle_incoming_server(pkt);
      else {
        dump_pkt(pkt, pkt->len());
        rte_pktmbuf_free(pkt);
      }
    }
  }

  void add_mac(uint32_t ip, rte_ether_addr &mac) {
    pkt_if.add_mapping(ip, mac);
  }

  connection *open_connection(const con_config &source,
                              const con_config &target) {
    flow_tuple ft(target.ip, source.ip, rte_cpu_to_be_16(target.port),
                  rte_cpu_to_be_16(source.port));
    FASTT_LOG_DEBUG("Opened new connection to %d %d\n", ft.sip,
                    rte_be_to_cpu_16(ft.sport));
    auto [it, inserted] = flows.emplace(
        ft, std::make_unique<connection>(allocator.get(), &pkt_if, target,
                                         source.port, this, is_client));
    if (!inserted)
      return nullptr;
    it->get()->open_connection();
    active.push_front(*it->get());
    ++open_connections;
    flush();
    return it->get();
  }

  template <typename F> void poll(F &&cb) {
    fetch_from_device();
    accept_connection();
    for (auto &con : active) {
      auto &inprogress_list = con.inprogress;
      auto it = inprogress_list.begin();
      auto end = inprogress_list.end();
      for (; it != end;) {
        auto ts = it++;
        cb(*ts);
      }
    }
    rte_timer_manage();
  }

  void poll_open_connection() {
    fetch_from_device();
    con_timer_manager.manage();
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
    connection_requests.emplace_back(pkt, ft);
  }

  connection *accept_connection() {
    if (connection_requests.empty())
      return nullptr;
    auto [pkt, ft] = connection_requests.front();
    auto [con, inserted] = add_connection(ft, rte_be_to_cpu_16(ft.dport));
    connection_requests.pop_front();
    con->handle_incoming_server(pkt);
    if (inserted) {
      con->accept();
      FASTT_LOG_DEBUG("Added new connection from %u %d\n", ft.sip, ft.sport);
    }
    return con;
  }
  std::pair<connection *, bool> add_connection(const flow_tuple &tuple,
                                               uint16_t port) {
    auto [it, inserted] = flows.emplace(
        tuple, std::make_unique<connection>(
                   allocator.get(), &pkt_if,
                   con_config{tuple.sip, rte_be_to_cpu_16(tuple.sport)}, port,
                   this, is_client));
    if (inserted) {
      active.push_front(*it->get());
      ++open_connections;
    }
    return {it->get(), inserted};
  }

  std::vector<statistics> get_stats() {
    std::vector<statistics> stats(open_connections);
    uint32_t i = 0;
    for (auto &con : active)
      stats[i++] = con.transport_impl->get_stats();
    return stats;
  }

  void flush() { scheduler.flush(); }

  ~connection_manager() {
    flush_timer.stop();
    ;
  }

private:
  static void flush_cb(rte_timer *timer, void *arg) {
    (void)timer;
    auto *this_ptr = static_cast<connection_manager *>(arg);
    this_ptr->flush();
  }
  std::deque<std::pair<message *, flow_tuple>> connection_requests;
  fixed_size_hash_table<flow_tuple, std::unique_ptr<connection>> flows;
  std::shared_ptr<message_allocator> allocator;
  netdev dev;
  packet_scheduler scheduler;
  packet_if pkt_if;
  intrusive_list_t<connection> active;
  bool is_client;
  uint32_t open_connections = 0;
  uint64_t flush_timeout;
  timer<dpdk_timer> flush_timer;
  timer_manager<dpdk_timer> con_timer_manager;
};
