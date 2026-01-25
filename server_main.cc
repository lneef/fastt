#include "connection.h"
#include "iface.h"
#include "message.h"
#include "server.h"
#include "transport/slot.h"
#include <arpa/inet.h>
#include <bits/getopt_core.h>
#include <cstdint>
#include <cstdlib>
#include <getopt.h>
#include <memory>
#include <rte_ether.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>

struct netconfig {
  rte_ether_addr dmac;
  uint32_t sip, dip;
  uint16_t sport, dport;
};

static netconfig parse_cmdline(int argc, char *argv[]) {
  int opt, option_index;
  netconfig conf;
  static const struct option long_options[] = {
      {"dip", required_argument, 0, 0},   {"sip", required_argument, 0, 0},
      {"dmac", required_argument, 0, 0},  {"sport", required_argument, 0, 0},
      {"dport", required_argument, 0, 0}, {0, 0, 0, 0}};
  while ((opt = getopt_long(argc, argv, "", long_options, &option_index)) !=
         -1) {
    switch (option_index) {
    case 0:
      conf.dip = inet_addr(optarg);
      break;
    case 1:
      conf.sip = inet_addr(optarg);
      break;
    case 2:
      rte_ether_unformat_addr(optarg, &conf.dmac);
      break;
    case 3:
      conf.sport = atoi(optarg);
      break;
    case 4:
      conf.dport = atoi(optarg);
      break;
    }
  }
  return conf;
}

int run(netconfig &conf) {
  rte_log_set_global_level(RTE_LOG_DEBUG);
  if (fastt::init())
    return -1;
  auto ifc = iface::configure_port(0, 1, 1);
  if (!ifc)
    return -1;
  auto [port, txq, rxq, pool] = ifc->get_slice(0);
  std::shared_ptr<message_allocator> allocator =
      std::make_shared<message_allocator>("pool", 8095);
  server_iface server(port, txq, rxq, con_config{conf.sip, conf.sport},
                      allocator);
  while (true) {
    server.poll([](transaction_slot &slot) {
      auto *msg = slot.rx_if.read();
      slot.tx_if.send(msg, true);
      if (!slot.has_outstanding_messages())
        slot.finish();
    });
    server.complete();
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int dpdk_argc = rte_eal_init(argc, argv);
  auto conf = parse_cmdline(argc - dpdk_argc, argv + dpdk_argc);
  run(conf);
  rte_eal_cleanup();
  return 0;
}
