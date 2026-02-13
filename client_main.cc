#include "client.h"
#include "connection.h"
#include "iface.h"
#include "kv.h"
#include "kv_protocol.h"
#include "message.h"
#include "util.h"
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
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
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
  std::vector<std::shared_ptr<message_allocator>> allocator;
  con_config cfg;
  rte_ether_addr dmac;

  lcore_adapter(std::size_t n, con_config cfg)
      : cifs(n), allocator(n, nullptr), cfg(cfg) {}
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

static constexpr auto dur = 1e6;
static constexpr uint16_t dataSize = sizeof(kv::kv_packet<kv::kv_request>);

static int lcore_fn(void *arg) {
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<int64_t> dist(INT64_MIN, INT64_MAX);
  auto *adapter = static_cast<lcore_adapter *>(arg);
  auto me = rte_lcore_index(rte_lcore_id());
  auto &allocator = adapter->allocator[me];
  auto &cif = *adapter->cifs[me];
  kv_proxy kv(&cif);
  kv.connect(adapter->cfg, 1, adapter->dmac);
  uint64_t t = 0;
  uint64_t c = 0;
  auto completion_handler = [&](slot &slt) {
      auto *msg = slt.get();
      if(!slt.get())
          return;
      slt.take();
      msg->free();
      if(slt.complete())
        slt.con->put_slot(&slt);
      ++c;
    };
  auto now = rte_get_timer_cycles();
  while (t < dur) {
    kv.handle_active(completion_handler);
    auto *tx = kv.start();
    if (!tx)
      continue;
    auto *req = allocator->alloc_message(dataSize);
    kv.lookup(dist(rng), req, t);
    auto sent = tx->send(req);
    assert(sent);
    ++t;
  }
  while (c < t) 
    kv.handle_active(completion_handler);  
  
  kv.acknowledge_all();
  kv.flush();
  auto stats = kv.con_at(0).get_transport_stats();
  std::cerr << stats.retransmitted << ", " << stats.retransmissions << std::endl;
  auto end = rte_get_timer_cycles();
  std::cerr << (end - now) / (rte_get_timer_hz() / 1e6) << std::endl;
  return 0;
}

static void run(lcore_function_t *f, void *args) {
  rte_eal_mp_remote_launch(f, args, CALL_MAIN);
  rte_eal_mp_wait_lcore();
}

int run(netconfig &conf) {
  if (fastt::init())
    return -1;
  auto cnt = rte_lcore_count();
  auto ifc = iface::configure_port(0, cnt, cnt);
  if (!ifc)
    return -1;

  uint16_t i = 0;
  uint16_t lcore;
  lcore_adapter adpater(rte_lcore_count(), {conf.dip, conf.sports[i]});
  RTE_LCORE_FOREACH(lcore) {
    auto [port, txq, rxq, pool] = ifc->get_slice(i);
    adpater.allocator[i] = std::make_shared<message_allocator>(
        ("mpool" + std::to_string(i)).c_str(), 8191);
    adpater.cifs[i] = std::make_unique<client_iface>(
        port, txq, rxq, adpater.allocator[i],
        con_config{conf.sip, conf.sports[i]}, lcore);
    adpater.dmac = conf.dmac;
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
