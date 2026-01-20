#include "client.h"
#include "iface.h"
#include "message.h"
#include <arpa/inet.h>
#include <bits/getopt_core.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <generic/rte_cycles.h>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <vector>
#include <atomic>

alignas(RTE_CACHE_LINE_MIN_SIZE)  std::atomic<double> lat = 0;

struct netconfig {
  rte_ether_addr dmac;
  uint32_t sip, dip;
  uint16_t sport, dport;
  uint32_t nports;
};

struct lcore_adapter {
  std::vector<std::unique_ptr<client_iface>> cifs;
  std::vector<connection *> connections;
  std::vector<std::shared_ptr<message_allocator>> allocator;

  lcore_adapter(std::size_t n)
      : cifs(n), connections(n), allocator(n, nullptr) {}
};

static netconfig parse_cmdline(int argc, char *argv[]) {
  int opt, option_index;

  netconfig conf;
  static const struct option long_options[] = {
      {"dip", required_argument, 0, 0},
      {"sip", required_argument, 0, 0},
      {"dmac", required_argument, 0, 0},
      {"sport", required_argument, 0, 0},
      {"dport", required_argument, 0, 0},
      {"nports", required_argument, 0, 0},
      {0, 0, 0, 0}};
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
    case 5:
      conf.nports = atoi(optarg);
      break;
    }
  }
  return conf;
}

static constexpr auto dur = 10000;
static constexpr uint16_t dataSize = 64;

static int lcore_fn(void *arg) {
  auto *adapter = static_cast<lcore_adapter *>(arg);
  auto me = rte_lcore_index(rte_lcore_id());
  auto *con = adapter->connections[me];
  auto &allocator = adapter->allocator[me];
  auto &cif = *adapter->cifs[me];
  auto pkts = 0;
  uint64_t sentt = 0;
  uint64_t total = 0;
  while (pkts < dur) {
    auto *msg = allocator->alloc_message(dataSize);
    auto *data = static_cast<char *>(msg->data());
    uint64_t now = rte_get_timer_cycles();
    memcpy(data, &now, sizeof(now));
    cif.send_message(con, msg, msg->len());
    cif.flush();
    msg = nullptr;
    do {
      msg = cif.recv_message(con);
      cif.flush();
    } while (!msg);
    now = rte_get_timer_cycles();
    memcpy(&sentt, msg->data(), sizeof(sentt));
    total += (now - sentt);
    ++pkts;
    rte_pktmbuf_free(msg);
  }
  lat += total / (static_cast<double>(rte_get_timer_hz()) / 1e6);
  return 0;
}

static void run(lcore_function_t *f, void *args) {
  rte_eal_mp_remote_launch(f, args, CALL_MAIN);
  rte_eal_mp_wait_lcore();
}

int run(netconfig &conf) {
  if (fastt::init())
    return -1;
  std::vector<iface> ifaces(conf.nports);
  lcore_adapter adpater(conf.nports);
  for (auto i = 0u; i < conf.nports; ++i) {
    auto ifc = iface::configure_port(0, 1, 1);
    if (!ifc)
      return -1;
    ifaces[i] = std::move(*ifc);

    auto [port, txq, rxq, pool] = ifc->get_slice(0);
    adpater.allocator[i] = std::make_shared<message_allocator>("pool", 8095);
    adpater.cifs[i] = std::make_unique<client_iface>(port, txq, rxq,
                                             adpater.allocator[i],
                                             con_config{conf.sip, conf.sport});
    auto& cif = adpater.cifs[i];
    auto *con = cif->open_connection({conf.dip, conf.dport}, conf.dmac);
    if (!con)
      return -1;
    while (!cif->probe_connection_setup_done(con))
      ;
    con->acknowledge_all();
  }
  run(lcore_fn, &adpater);
  std::cout << "avg: " << lat.load() / rte_lcore_count() << std::endl;
  return 0;
}

int main(int argc, char *argv[]) {
  int dpdk_argc = rte_eal_init(argc, argv);
  auto conf = parse_cmdline(argc - dpdk_argc, argv + dpdk_argc);
  run(conf);
  rte_eal_cleanup();
  return 0;
}
