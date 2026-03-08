#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <netinet/in.h>
#include <sys/types.h>
#include <type_traits>
#include <utility>

#include "debug.h"
#include "dev.h"
#include "dpdk/allocator.h"
#include "packet_if.h"
#include "sgl.h"
#include "slab_allocator.h"
#include "task/task.h"
#include "transport/protocol.h"
#include "transport/transport.h"
#include "util.h"

class iface;
class server_iface;
class client_iface;
class connection_manager;

struct statistics {
  std::vector<transport_statistics> ts;
  uint64_t total_rx_polled = 0, no_rx = 0;
};

class connection {
public:
  connection(packet_if *pkt_if, slab_allocator *sb, const transport_config &cfg,
             uint16_t sport, uint16_t dport, connection_manager *manager,
             bool is_client)
      : transport_impl(
            std::make_unique<transport<>>(pkt_if, sb, cfg, sport, dport)),
        manager(manager), is_client(is_client) {}
  void process_pkt(mbuf *pkt);
  void acknowledge_all();
  void accept();
  void open_connection(uint16_t rx_flow_sport, uint16_t rx_flow_dport);

  void check_timeout(uint64_t now) { transport_impl->check_timeout(now); }

  ssize_t send(sgl &msgl){
      return transport_impl->send_sgl(msgl);
  }

  ssize_t recv(sgl& msgl){
      return transport_impl->recv(msgl);
  }

  transport_statistics get_transport_stats() const {
    return transport_impl->get_stats();
  }


  bool up() const { return transport_impl->up(); }

  bool disconnecting() const {
    return transport_impl->get_state() == connection_state::DISCONNECTING;
  }

  bool down() const { return transport_impl->disconnected(); }

  bool can_send() { return transport_impl->can_send(); }

  bool can_recv() { return transport_impl->can_recv(); }

  connection_manager *get_manager() { return manager; }

  void perform_recovery() { transport_impl->perform_recovery(); }

  flow_tuple get_flow_tuple() const { return transport_impl->get_flow_tuple(); }

  void close() { transport_impl->close_connection(); }

  bool done() const { return transport_impl->all_acked(); }

private:
  friend class connection_manager;
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
  template <typename P>
  connection_manager(bool is_client, uint16_t port, uint16_t txq, uint16_t rxq,
                     uint32_t sip,
                     std::shared_ptr<dpdk_allocator> allocator,
                     P *parent, uint16_t cores)
      : dev(port, txq, rxq), 
        pkt_if(&dev, allocator, &sb, sip, port), active(), cores(cores),
        is_client(is_client) {
    if constexpr (std::is_same_v<client_iface, P>)
      client_parent = parent;
    else
      server_parent = parent;
  }

  void handle_pkt(mbuf *pkt, flow_tuple &ft) {
    FASTT_LOG_DEBUG("Got pkt via UDP ports: %s \n", ft.print().c_str());
    auto *header = pkt->data<protocol::ft_header>();
    if (unlikely(header->type == protocol::FT_SYN)) {
      FASTT_LOG_DEBUG("Registering new request");
      connection_requests.emplace_back(pkt, ft);
    } else {
      protocol::extract_ports(ft, pkt);
      FASTT_LOG_DEBUG("Got packet via %s\n", ft.print().c_str());
      auto it = flows.find(ft);
      if (likely(it != flows.end()))
        it->second->process_pkt(pkt);
      else {
        mbuf_free(pkt);
      }
    }
  }

  void check_timeouts() {
    for (auto &con : active) {
      auto now = rte_get_timer_cycles();
      con.check_timeout(now);
    }
  }

  void acknowledge() {
    for (auto &con : active) {
      if (con.disconnecting())
        continue;
      con.acknowledge_all();
    }
  }

  void add_mac(uint32_t ip, rte_ether_addr &mac) {
    pkt_if.add_mapping(ip, mac);
  }

  connection *open_connection(uint16_t sport, uint16_t dport,
                              const uint32_t sip, const uint32_t dip,
                              const uint16_t target);

  void poll_client() {
    fetch_from_qpair();
    for (auto it = active.begin(), end = active.end(); it != end;) {
      auto &con = *it;
      ++it;
      con.perform_recovery();
      con.acknowledge_all();
      if (con.down())
        con.link.unlink();
    }
    check_timeouts();
    flush();
  }

  void run(concurrency::scheduler &scheduler);

  void fetch_from_qpair() {
    std::array<flow_tuple, packet_if::kDefaultInBurstSize> fts;
    packet_vector<mbuf*, packet_if::kDefaultInBurstSize> mbufs;
    pkt_if.fetch_from_qpair(fts, mbufs);
    uint16_t i = 0;
    for (auto* pkt : mbufs) {
      auto &ft = fts[i++];
      handle_pkt(pkt, ft);
    }
  }

  template <typename F> void accept_connections(F &&cb) {
    while (!connection_requests.empty()) {
      auto [pkt, ft] = connection_requests.front();
      connection_requests.pop_front();
      auto [con, inserted] = add_connection(ft, pkt);
      con->process_pkt(pkt);
      if (inserted) {
        con->accept();
        FASTT_LOG_DEBUG("Added new connection from %u %d\n", ft.sip, ft.sport);
        cb(con);
      }
    }
  }

  std::pair<connection *, bool> add_connection(flow_tuple &tuple, mbuf *pkt) {
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
        tuple, std::make_unique<connection>(&pkt_if, &sb, cfg, tuple.dport,
                                            tuple.sport, this, is_client));
    if (inserted) {
      active.push_front(*it->second);
      ++open_connections;
    } else if (it->second->down()) {
      // if the connection has been closed, replace it
      it->second.reset();
      it->second = std::make_unique<connection>(&pkt_if, &sb, cfg, tuple.dport,
                                                tuple.sport, this, is_client);
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
    sts.ts = std::move(stats);
    return sts;
  }

  void close(connection *con) {
    auto ft = con->get_flow_tuple();
    ft.dip = pkt_if.get_sip();
    flows.erase(ft);
  }

  slab_allocator* get_allocator(){
      return &sb;
  }

  void flush() { pkt_if.flush_out_buffer(); }

  ~connection_manager() {}

private:
  std::deque<std::pair<mbuf *, flow_tuple>> connection_requests;
  qpair dev;
  slab_allocator sb;
  packet_if pkt_if;
  intrusive_list_t<connection> active;
  uint16_t cores;
  bool is_client;
  uint32_t open_connections = 0;

  flow_table<flow_tuple, std::unique_ptr<connection>> flows;
  union {
    client_iface *client_parent;
    server_iface *server_parent;
  };
};
