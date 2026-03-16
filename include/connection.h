#pragma once

#include <cstdint>
#include <deque>
#include <generic/rte_cycles.h>
#include <memory>
#include <netinet/in.h>
#include <sys/types.h>
#include <type_traits>
#include <utility>

#include "debug.h"
#include "dev.h"
#include "dpdk/allocator.h"
#include "packet_if.h"
#include "slab_allocator.h"
#include "task/task.h"
#include "transport/protocol.h"
#include "transport/transport.h"
#include "util.h"

class iface;
class server_iface;
class client_iface;
class connection_manager;

using connection = transport<packet_if, connection_manager>;

struct statistics {
  std::vector<transport_statistics> ts;
  uint64_t total_rx_polled = 0, no_rx = 0;
};

class connection_manager {
  static constexpr uint16_t kdefaultBurstSize = 64;
  friend connection;

public:
  template <typename P>
  connection_manager(bool is_client, uint16_t port, uint16_t txq, uint16_t rxq,
                     uint32_t sip, std::shared_ptr<dpdk_allocator> allocator,
                     P *parent, uint16_t cores)
      : dev(port, txq, rxq), pkt_if(&dev, allocator, &sb, sip, port), active(),
        cores(cores), is_client(is_client) {
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
      if (likely(it != flows.end())) {
        it->second->process_pkt(pkt);
        if (!it->second->ready.is_linked())
          ready.push_back(*it->second);
        if (!it->second->ack_outstanding.is_linked())
          ack_outstanding.push_back(*it->second);

      } else {
        mbuf_free(pkt);
      }
    }
  }

  void check_timeouts() {
    for (auto &con : active)
      con.check_timeout(r_ts);
  }

  void acknowledge() {
    for (auto &con : active) {
      if (con.get_state() == connection_state::DISCONNECTING)
        continue;
      con.acknowledge();
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
    update_current_timer_cycles();
    for (size_t i = 0u, end = ack_outstanding.size(); i < end; ++i) {
      auto &con = ack_outstanding.front();
      ack_outstanding.pop_front();
      if (con.acknowledge())
        ack_outstanding.push_back(con);
    }
    flush();

    for (auto& con: ready) {
      con.perform_recovery();
      if (con.get_state() == connection_state::DISCONNECTED)
        con.link.unlink();
    }
    ready.clear();

    check_timeouts();
  }

  void run(concurrency::scheduler &scheduler);

  void fetch_from_qpair() {
    std::array<flow_tuple, packet_if::kDefaultInBurstSize> fts;
    packet_vector<mbuf *, packet_if::kDefaultInBurstSize> mbufs;
    pkt_if.fetch_from_qpair(fts, mbufs);
    uint16_t i = 0;
    for (auto *pkt : mbufs) {
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
        con->accept_connection();
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
        tuple, std::make_unique<connection>(&pkt_if, &sb, this, cfg,
                                            tuple.dport, tuple.sport));
    if (inserted) {
      active.push_front(*it->second);
      ++open_connections;
    } else if (it->second->get_state() == connection_state::DISCONNECTED) {
      // if the connection has been closed, replace it
      it->second.reset();
      it->second = std::make_unique<connection>(&pkt_if, &sb, this, cfg,
                                                tuple.dport, tuple.sport);
      active.push_front(*it->second);
      inserted = true;
    }
    return {it->second.get(), inserted};
  }

  statistics get_stats() {
    std::vector<transport_statistics> stats(open_connections);
    uint32_t i = 0;
    for (auto &timpl : active)
      stats[i++] = timpl.get_stats();
    statistics sts;
    sts.ts = std::move(stats);
    return sts;
  }

  void close(connection *con) {
    auto ft = con->get_flow_tuple();
    ft.dip = pkt_if.get_sip();
    flows.erase(ft);
  }

  slab_allocator *get_allocator() { return &sb; }

  __inline uint64_t get_current_timer_cycles() const { return r_ts; }

  void update_current_timer_cycles() { r_ts = rte_get_timer_cycles(); }

  void flush() { pkt_if.flush_out_buffer(); }

  ~connection_manager() {}

private:
  std::deque<std::pair<mbuf *, flow_tuple>> connection_requests;
  qpair dev;
  slab_allocator sb;
  packet_if pkt_if;
  uint64_t r_ts = rte_get_timer_cycles();
  intrusive_list_t<connection> active;
  intrusive_list_t<connection, &connection::ready> ready;
  intrusive_list_t<connection, &connection::ack_outstanding> ack_outstanding;
  uint16_t cores;
  bool is_client;
  uint32_t open_connections = 0;

  flow_table<flow_tuple, std::unique_ptr<connection>> flows;
  union {
    client_iface *client_parent;
    server_iface *server_parent;
  };
};
