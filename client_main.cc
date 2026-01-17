#include "client.h"
#include "iface.h"
#include "message.h"
#include <arpa/inet.h>
#include <bits/getopt_core.h>
#include <cstdint>
#include <cstdlib>
#include <getopt.h>
#include <rte_eal.h>
#include <rte_ether.h>
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
  static constexpr uint16_t dataSize = 64;
  if (fastt::init())
    return -1;
  auto ifc = iface::configure_port(0, 1, 1);
  if (!ifc)
    return -1;
  auto [port, txq, rxq, pool] = ifc->get_slice(0);
  std::shared_ptr<message_allocator> allocator =
      std::make_shared<message_allocator>("pool", 8095);
  client_iface cif{port, txq, rxq, allocator, con_config{conf.sip, conf.sport}};
  auto *con = cif.open_connection({conf.dip, conf.dport}, conf.dmac);
  if (!con)
    return -1;
  while(!con->active())
      cif.recv_message(con);
  con->acknowledge_all();
  while (true) {
    auto *msg = allocator->alloc_message(dataSize);
    cif.send_message(con, msg, msg->len());
    cif.flush();
    msg = nullptr;
    do {
      msg = cif.recv_message(con);
    } while (!msg);
    allocator->deallocate(msg);
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
