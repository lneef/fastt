#include "connection.h"
#include "debug.h"
#include "server.h"
#include "task/task.h"
#include "transport/transport.h"

#include <cassert>
#include <cstdint>
#include <generic/rte_cycles.h>
#include <netinet/in.h>
#include <random>

connection *connection_manager::open_connection(uint16_t sport, uint16_t dport,
                                                const uint32_t sip,
                                                const uint32_t dip) {

  std::random_device rnd;
  std::mt19937 rng(rnd());
  std::uniform_int_distribution<uint16_t> dist{0, UINT16_MAX};
  uint16_t rx_flow_sport, rx_flow_dport;
  transport_config cfg;
  cfg.ip = dip;
  sport = htons(sport);
  dport = htons(dport);
  flow_tuple ft(cfg.ip, sip, dport, sport);
  FASTT_LOG_DEBUG("Opened new connection to %d %d\n", ft.sip, ntohs(ft.sport));
  cfg.transport_ports.sport = dist(rng);
  cfg.transport_ports.dport = dist(rng);
  // find transport level queue pair
  dev.nic_arch->find_port_pair(cfg.ip, sip, rx_flow_sport, rx_flow_dport,
                               dev.get_rx_qid(), cores);
  FASTT_LOG_DEBUG("Found pair for incoming: %u -> %u\n",
                  ntohs(cfg.transport_ports.dport),
                  ntohs(cfg.transport_ports.sport));
  auto [it, inserted] = flows.emplace(
      ft, std::make_unique<connection>(&pkt_if, &sb, this, cfg, sport, dport));
  if (!inserted)
    return nullptr;
  it->second->open_connection(rx_flow_sport, rx_flow_dport);
  active.push_front(*it->second);
  ++open_connections;
  flush();
  return it->second.get();
}

uint64_t connection_manager::run_loop_head(concurrency::scheduler &scheduler) {
  update_current_timer_cycles();
  fetch_from_qpair();
  accept_connections([&](connection *con) {
    assert(server_parent->services.find(ntohs(con->get_flow_tuple().sport)) !=
           server_parent->services.end());
    auto service_handler =
        server_parent->services[ntohs(con->get_flow_tuple().sport)];
    assert(!is_client);
    scheduler.schedule(service_handler(*server_parent, *con).handle);
  });

  for (size_t i = 0u, end = ack_outstanding.size(); i < end; ++i) {
    auto &con = ack_outstanding.front();
    ack_outstanding.pop_front();
    if (con.acknowledge())
      ack_outstanding.push_back(con);
  }
  flush();
  return r_ts;
}

void connection_manager::run(concurrency::scheduler &scheduler) {
  update_current_timer_cycles();
  fetch_from_qpair();
  accept_connections([&](connection *con) {
    assert(server_parent->services.find(ntohs(con->get_flow_tuple().sport)) !=
           server_parent->services.end());
    auto service_handler =
        server_parent->services[ntohs(con->get_flow_tuple().sport)];
    assert(!is_client);
    scheduler.schedule(service_handler(*server_parent, *con).handle);
  });

  update_current_timer_cycles();
  for (size_t i = 0u, end = ack_outstanding.size(); i < end; ++i) {
    auto &con = ack_outstanding.front();
    ack_outstanding.pop_front();
    if (con.acknowledge())
      ack_outstanding.push_back(con);
  }

  flush();
  auto ready_num = ready.size();
  for (unsigned i = 0; i < ready_num; ++i) {
    auto &con = ready.front();
    ready.pop_front();
    con.perform_recovery();
    concurrency::make_progress(con);
  }

  scheduler.run([&]{ run_loop_head(scheduler); });
  check_timeouts();
}
