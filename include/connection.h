#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <netinet/in.h>
#include <ranges>
#include <utility>

#include "debug.h"
#include "dev.h"
#include "message.h"
#include "packet_if.h"
#include "task.h"
#include "transport/protocol.h"
#include "transport/transport.h"
#include "util.h"

class iface;
class connection_manager;
class coro_handle;

struct statistics {
  std::vector<transport_statistics> ts;
  uint64_t total_rx_polled = 0, no_rx = 0;
};

class connection {
  static constexpr uint16_t kMaxSlotsPerConnection = 128;

public:
  connection(message_allocator *allocator, packet_if *pkt_if,
             const transport_config &cfg, uint16_t sport, uint16_t dport,
             connection_manager *manager, bool is_client)
      : allocator(allocator), transport_impl(std::make_unique<transport<>>(
                                  allocator, pkt_if, cfg, sport, dport)),
        manager(manager), is_client(is_client) {}
  void process_pkt(rte_mbuf *pkt);
  void acknowledge_all(uint64_t now);
  void accept();
  void open_connection();

  void check_timeout(uint64_t now) { transport_impl->check_timeout(now); }

  size_t send(msg_hdr &hdr) { return transport_impl->send(hdr); }

  ssize_t recv(msg_hdr &hdr) { return transport_impl->recv(hdr); }

  concurrency::send_awaitable send(concurrency::scheduler &schdlr, message *msg,
                                   bool first, bool last);

  concurrency::recv_awaitable recv(concurrency::scheduler &schdlr,
                                   message **msg);

  transport_statistics get_transport_stats() const {
    return transport_impl->get_stats();
  }

  void transport_ctrl() { transport_impl->check_ctrl(); }

  void make_progress();

  bool up() const { return transport_impl->up(); }

  bool down() const { return transport_impl->disconnected(); }

  bool can_send() { return transport_impl->can_send(); }

  bool can_recv() { return transport_impl->can_recv(); }

  connection_manager *get_manager() { return manager; }

  flow_tuple get_flow_tuple() const { return transport_impl->get_flow_tuple(); }

  void close() { transport_impl->close_connection(); }

private:
  friend class connection_manager;
  message_allocator *allocator;
  std::unique_ptr<transport<>> transport_impl;
  connection_manager *manager;
  bool is_client;

public:
  std::optional<concurrency::coro_handle> coro;
  list_hook link;
};

class connection_manager {
  static constexpr uint16_t kdefaultBurstSize = 64;

public:
  connection_manager(bool is_client, uint16_t port, uint16_t txq, uint16_t rxq,
                     uint32_t sip, std::shared_ptr<message_allocator> allocator)
      : allocator(allocator), dev(port, txq, rxq), scheduler(&dev),
        pkt_if(&scheduler, sip, port), active(), is_client(is_client),
        flush_timeout(get_ticks_us()) {}

  void handle_pkt(message *pkt, flow_tuple &ft) {
      
    FASTT_LOG_DEBUG("Got pkt via UDP ports: %s \n", ft.print().c_str());
    auto *header = rte_pktmbuf_mtod(pkt, protocol::ft_header *);
    if (unlikely(header->type == protocol::FT_RDY_TO_RCV))
      register_request(pkt, ft);
    else {
      protocol::extract_ports(ft, pkt);
      FASTT_LOG_DEBUG("Got packet via %s\n", ft.print().c_str());
      auto it = flows.find(ft);
      if (likely(it != flows.end()))
        it->second->process_pkt(pkt);
      else {
        dump_pkt(pkt, pkt->len());
        rte_pktmbuf_free(pkt);
      }
    }
  }

  void check_timeouts() {
    auto now = rte_get_timer_cycles();
    for (auto &con : active) {
      con.transport_ctrl();
      con.check_timeout(now);
    }
  }

  void acknowledge_all_and_reap() {
    auto now = rte_get_timer_cycles();
    for (auto it = active.begin(), end = active.end(); it != end;) {
      auto &con = *it;
      ++it;
      con.acknowledge_all(now);
      if (con.down())
        con.link.unlink();
    }
  }

  void add_mac(uint32_t ip, rte_ether_addr &mac) {
    pkt_if.add_mapping(ip, mac);
  }

  connection *open_connection(uint16_t sport, uint16_t dport,
                              const uint32_t sip, const uint32_t dip,
                              const uint16_t target) {
    transport_config cfg;
    cfg.ip = dip;
    sport = htons(sport);
    dport = htons(dport);
    flow_tuple ft(cfg.ip, sip, dport, sport);
    FASTT_LOG_DEBUG("Opened new connection to %d %d\n", ft.sip,
                    ntohs(ft.sport));

    // find transport level queue pair
    dev.nic_arch->find_port_pair(cfg.ip, sip, cfg.transport_ports.dport,
                                 cfg.transport_ports.sport, target);
    FASTT_LOG_DEBUG("Found pair for incoming: %u -> %u\n", ntohs(cfg.transport_ports.dport),
                    ntohs(cfg.transport_ports.sport));
    auto [it, inserted] = flows.emplace(
        ft, std::make_unique<connection>(allocator.get(), &pkt_if, cfg, sport,
                                         dport, this, is_client));
    if (!inserted)
      return nullptr;
    it->second->open_connection();
    active.push_front(*it->second);
    ++open_connections;
    flush();
    return it->second.get();
  }

  template <typename F> void poll(F &&handler) {
    fetch_from_qpair();
    accept_connection();
    flush();
    for (auto &con : active)
      handler(con);
    acknowledge_all_and_reap();
    check_timeouts();
  }

  void poll_client() {
    fetch_from_qpair();
    acknowledge_all_and_reap();
    check_timeouts();
    flush();
  }

  template <typename S, typename F> void run(S &scheduler, F &&handler) {
    fetch_from_qpair();
    if (!is_client)
      accept_connection();
    scheduler.run();
  }

  void fetch_from_qpair() {
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
    auto [con, inserted] = add_connection(ft, pkt);
    con->process_pkt(pkt);
    if (inserted) {
      con->accept();
      FASTT_LOG_DEBUG("Added new connection from %u %d\n", ft.sip, ft.sport);
    }
    return con;
  }

  std::pair<connection *, bool> add_connection(flow_tuple &tuple,
                                               message *pkt) {
    transport_config cfg;
    cfg.ip = tuple.sip;
    cfg.transport_ports.dport = tuple.sport;
    cfg.transport_ports.sport = tuple.dport;
    FASTT_LOG_DEBUG("Found pair: %u -> %u\n", ntohs(cfg.transport_ports.sport),
                    ntohs(cfg.transport_ports.dport));
    protocol::extract_ports(tuple, pkt);
    FASTT_LOG_DEBUG("New Connection %s \n", tuple.print().c_str());
    // swap ports since we need the rx port as src
    auto [it, inserted] = flows.emplace(
        tuple,
        std::make_unique<connection>(allocator.get(), &pkt_if, cfg, tuple.dport,
                                     tuple.sport, this, is_client));
    if (inserted) {
      active.push_front(*it->second);
      ++open_connections;
    } else if (it->second->down()) {
      // if the connection has been closed, replace it
      it->second.reset();
      it->second = std::make_unique<connection>(allocator.get(), &pkt_if, cfg,
                                                tuple.dport, tuple.sport, this,
                                                is_client);
      active.push_front(*it->second);
      inserted = true;
    }
    return {it->second.get(), inserted};
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

  void close(connection *con) {
    auto ft = con->get_flow_tuple();
    ft.dip = pkt_if.get_sip();
    flows.erase(ft);
  }

  void flush() { scheduler.flush(); }

  ~connection_manager() {}

private:
  std::shared_ptr<message_allocator> allocator;
  std::deque<std::pair<message *, flow_tuple>> connection_requests;
  flow_table<flow_tuple, std::unique_ptr<connection>> flows;
  qpair dev;
  packet_scheduler scheduler;
  packet_if pkt_if;
  intrusive_list_t<connection> active;
  bool is_client;
  uint32_t open_connections = 0;
  uint64_t flush_timeout;
  packet_vector<kdefaultBurstSize> vec;
};
