#include "connection.h"
#include "debug.h"
#include "msg_fragment.h"
#include "server.h"

#include <cassert>
#include <cstdint>
#include <netinet/in.h>
#include <random>

static std::mt19937 rng;
static std::uniform_int_distribution<uint16_t> dist{0, UINT16_MAX};

connection *connection_manager::open_connection(uint16_t sport, uint16_t dport,
                                                const uint32_t sip,
                                                const uint32_t dip,
                                                const uint16_t target) {
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
                               target, cores);
  FASTT_LOG_DEBUG("Found pair for incoming: %u -> %u\n",
                  ntohs(cfg.transport_ports.dport),
                  ntohs(cfg.transport_ports.sport));
  auto [it, inserted] = flows.emplace(
      ft, std::make_unique<connection>(&pkt_if, &sb, cfg, sport,
                                       dport, this, is_client));
  if (!inserted)
    return nullptr;
  it->second->open_connection(rx_flow_sport, rx_flow_dport);
  active.push_front(*it->second);
  ++open_connections;
  flush();
  return it->second.get();
}

void connection::process_pkt(mbuf *pkt) {
  transport_impl->process_pkt(pkt);
}

void connection::acknowledge_all() { transport_impl->acknowledge(); }

void connection::accept() { transport_impl->accept_connection(); }

void connection::open_connection(uint16_t rx_flow_sport,
                                 uint16_t rx_flow_dport) {
  transport_impl->open_connection(rx_flow_sport, rx_flow_dport);
}

void connection_manager::run(concurrency::scheduler &scheduler) {
  fetch_from_qpair();
  accept_connections([&](connection *con) {
    assert(server_parent->services.find(ntohs(con->get_flow_tuple().sport)) !=
           server_parent->services.end());
    auto service_handler =
        server_parent->services[ntohs(con->get_flow_tuple().sport)];
    scheduler.schedule(service_handler(scheduler, *con).handle);
  });
  flush();
  for (auto &con : active)
    concurrency::make_progress(con);
  scheduler.run();
  for (auto &con : active)
    con.acknowledge_all();
  check_timeouts();
}
