#include "client.h"
#include "iface.h"
#include "kv.h"
#include "message.h"
#include "transaction.h"
#include <arpa/inet.h>
#include <atomic>
#include <bits/getopt_core.h>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <generic/rte_cycles.h>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <random>
#include <ranges>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <string_view>
#include <vector>

alignas(RTE_CACHE_LINE_MIN_SIZE) std::atomic<double> lat = 0;

struct netconfig {
  rte_ether_addr dmac;
  uint32_t sip, dip;
  uint16_t dport;
  std::vector<uint16_t> sports;
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
    case 3: {
      auto ports = std::string(optarg);
      for (auto p : ports | std::ranges::views::split(':')) {
        auto sv = std::string_view(p.begin(), p.end());
        conf.sports.push_back(0);
        std::from_chars(sv.begin(), sv.end(), conf.sports.back());
      }
      break;
    }
    case 4:
      conf.dport = atoi(optarg);
      break;
    }
  }
  return conf;
}

static constexpr auto dur = 10000;
static constexpr uint16_t dataSize = 64;
static int lcore_fn(void *arg) {
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dist(INT64_MIN,
                                                                INT64_MAX);
  auto *adapter = static_cast<lcore_adapter *>(arg);
  transaction_queue queue{512};
  auto me = rte_lcore_index(rte_lcore_id());
  auto *con = adapter->connections[me];
  auto &allocator = adapter->allocator[me];
  auto &cif = *adapter->cifs[me];
  auto pkts = 0;
  uint64_t now = rte_get_timer_cycles();

  kv_proxy kv(&cif, con);
  while (pkts < dur) {
    message *msg;
    auto *req = allocator->alloc_message(dataSize);
    create_put_request(req, dist(rng), dist(rng));
    auto resp = kv.start_transaction(con, req, queue);
    assert(resp.get());
    resp->tx_if().send(req, true);
    kv.flush();
    resp->wait();
    msg = resp->rx_if().read();
    if (resp->finish()) {
      allocator->deallocate(msg);
      kv.finish_transaction(resp.get());
    }
    assert(resp->completed());
    ++pkts;
  }

  auto end = rte_get_timer_cycles();
  lat += (end - now) / (static_cast<double>(rte_get_timer_hz()) / 1e6) / pkts;
  return 0;
}

static void run(lcore_function_t *f, void *args) {
  rte_eal_mp_remote_launch(f, args, CALL_MAIN);
  rte_eal_mp_wait_lcore();
}

int run(netconfig &conf) {
  if (fastt::init())
    return -1;
  auto ifc = iface::configure_port(0, 1, 1);
  if (!ifc)
    return -1;

  uint16_t i = 0;
  lcore_adapter adpater(rte_lcore_count());
  for (auto p : conf.sports) {
    auto [port, txq, rxq, pool] = ifc->get_slice(i);
    adpater.allocator[i] = std::make_shared<message_allocator>("pool", 8095);
    adpater.cifs[i] = std::make_unique<client_iface>(
        port, txq, rxq, adpater.allocator[i], con_config{conf.sip, p});
    auto &cif = adpater.cifs[i];
    auto *con = cif->open_connection({conf.dip, conf.dport}, conf.dmac);
    if (!con)
      return -1;
    while (!cif->probe_connection_setup_done(con))
      ;
    con->acknowledge_all();
    adpater.connections[i] = con;
    ++i;
  }
  run(lcore_fn, &adpater);

  ifc->stop();
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
