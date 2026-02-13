#pragma once

#include <cstdint>
#include <deque>
#include <generic/rte_cycles.h>
#include <memory.h>
#include <memory>
#include <ranges>
#include <rte_branch_prediction.h>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_udp.h>
#include <utility>

#include "debug.h"
#include "dev.h"
#include "message.h"
#include "packet_if.h"
#include "protocol.h"
#include "slot.h"
#include "timer.h"
#include "transport/transport.h"
#include "util.h"

class iface;
class connection_manager;

struct statistics {
  std::vector<transport_statistics> ts;
  uint64_t total_rx_polled = 0, no_rx = 0;
};

class connection {
  static constexpr uint16_t kMaxSlotsPerConnection =
      transport::kOustandingMessages;

public:
  connection(message_allocator *allocator, packet_if *pkt_if,
             const con_config &target, uint16_t sport,
             connection_manager *manager, bool is_client)
      : allocator(allocator), transport_impl(std::make_unique<transport>(
                                  allocator, pkt_if, sport, target)),
        manager(manager), is_client(is_client) {

    slots.reserve(kMaxSlotsPerConnection);
    for (auto i = 0u; i < kMaxSlotsPerConnection; ++i) {
      slots.emplace_back(i, this);
      free_list.emplace_back(i);
    }
  }
  void process_pkt(rte_mbuf *pkt);
  void acknowledge_all();
  void accept();
  uint16_t receive_message(message **msgs, uint16_t cnt);
  void open_connection();

  unsigned capacity() { return transport_impl->capacity(); }

  void check_timeout(uint64_t now) { transport_impl->check_timeout(now); }

  bool send_pkt(message *msg, uint16_t sid, bool first, bool last) {
    return transport_impl->send_pkt(msg, sid, first, last);
  }

  void handle_incoming() {
    transport_impl->receive_messages([&](message *msg) {
      auto *hdr = msg->data<protocol::ft_header>();
      slots[hdr->sid].handle_incoming(msg);
      if (hdr->end && !is_client) {
        assert(!slots[hdr->sid].link.is_linked());
        slots[hdr->sid].move_to_active(active);
      }
      msg->shrink_headroom(sizeof(protocol::ft_header));
    });
  }

  transport_statistics get_transport_stats() const {
    return transport_impl->get_stats();
  }

  slot *get_slot() {
    assert(is_client);
    if (capacity() == 0 || free_list.empty())
      return nullptr;
    auto slt_num = free_list.front();
    free_list.pop_front();
    slots[slt_num].move_to_active(active);
    return &slots[slt_num];
  }

  void put_slot(slot *slt) {
    assert(slt->link.is_linked());
    free_list.push_front(slt->id);
    slt->unlink();
  }

  bool up() const { return transport_impl->active(); }

  connection_manager *get_manager() { return manager; }

private:
  friend class connection_manager;
  message_allocator *allocator;
  std::unique_ptr<transport> transport_impl;
  std::vector<slot> slots;
  std::deque<unsigned> free_list;
  connection_manager *manager;
  intrusive_list_t<slot> active;
  bool is_client;

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
    if (unlikely(header->type == protocol::FT_INIT))
      register_request(pkt, ft);
    else {
      auto *connection = flows.lookup(ft);
      if (likely(connection))
        (*connection)->process_pkt(pkt);
      else {
        dump_pkt(pkt, pkt->len());
        rte_pktmbuf_free(pkt);
      }
    }
  }

  void check_timeouts() {
    auto now = rte_get_timer_cycles();
    for (auto &con : active)
      con.check_timeout(now);
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

  template <typename F> void poll(F &&handler) {
    fetch_from_device();
    if (!is_client)
      accept_connection();
    for (auto &con : active) {
      con.handle_incoming();
      for (auto it = con.active.begin(), end = con.active.end(); it != end;) {
        auto &slt = *it;
        ++it;
        handler(slt);
      }
    }
    check_timeouts();
    con_timer_manager.manage();
  }

  void fetch_from_device() {
    std::array<flow_tuple, kdefaultBurstSize> fts;
    uint16_t valid = 0, i = 0;
    assert(vec.i == 0);
    dev.rx_burst(vec);
    for (uint16_t i = 0; i < vec.i; ++i) {
      auto *pkt = pkt_if.consume_pkt(vec.pkts[i]);
      if (!pkt)
        continue;
      vec.pkts[valid++] = pkt;
    }
    vec.i = valid;
    assert(i == 0);
    for (auto *msg : vec)
      pkt_if.strip_header(msg, fts[i++]);

    for (auto [msg, ft] : std::ranges::zip_view(vec, fts))
      handle_pkt(msg, ft);
    vec.clear();
    assert(vec.i == 0);
  }

  void register_request(message *pkt, flow_tuple &ft) {
    FASTT_LOG_DEBUG("Registering new request");
    connection_requests.emplace_back(pkt, ft);
  }

  connection *accept_connection() {
    if (connection_requests.empty())
      return nullptr;
    auto [pkt, ft] = connection_requests.front();
    connection_requests.pop_front();
    auto [con, inserted] = add_connection(ft, rte_be_to_cpu_16(ft.dport));
    con->process_pkt(pkt);
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

  statistics get_stats() {
    std::vector<transport_statistics> stats(open_connections);
    uint32_t i = 0;
    for (auto &con : active)
      stats[i++] = con.transport_impl->get_stats();
    statistics sts;
    sts.no_rx = dev.no_rx;
    sts.total_rx_polled = dev.total_rx;
    sts.ts = std::move(stats);
    return sts;
  }

  void flush() { scheduler.flush(); }

  ~connection_manager() { flush_timer.stop(); }

private:
  static void flush_cb(rte_timer *timer, void *arg) {
    (void)timer;
    auto *this_ptr = static_cast<connection_manager *>(arg);
    this_ptr->flush();
  }
  std::deque<std::pair<message *, flow_tuple>> connection_requests;
  fixed_size_hash_table<flow_tuple, std::unique_ptr<connection>> flows;
  std::shared_ptr<message_allocator> allocator;
  qpair dev;
  packet_scheduler scheduler;
  packet_if pkt_if;
  intrusive_list_t<connection> active;
  bool is_client;
  uint32_t open_connections = 0;
  uint64_t flush_timeout;
  packet_vector<kdefaultBurstSize> vec;
  timer<dpdk_timer> flush_timer;
  timer_manager<dpdk_timer> con_timer_manager;
};
