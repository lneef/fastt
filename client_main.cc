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
static int lcore_fn(void *arg) {
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<int64_t> dist(INT64_MIN, INT64_MAX);
  auto *adapter = static_cast<lcore_adapter *>(arg);
  auto me = rte_lcore_index(rte_lcore_id());
  auto &cif = *adapter->cifs[me];
  kv_proxy kv(&cif);
  kv.connect(adapter->cfg, 1, rte_lcore_id(), adapter->dmac);
  uint64_t t = 0;
  uint64_t c = 0;

  kv::kv_packet<kv::kv_request> req;
  kv::kv_packet<kv::kv_completion> resp;
  auto now = rte_get_timer_cycles();
  size_t rcvd = 0;
  while(t < dur){
      cif.poll();
      while((rcvd = kv.recv(&resp, sizeof(resp)) > 0)){
          assert(resp.payload.key == kv[resp.id].key);
          kv.complete(resp.id);
          ++c;
      }
      auto *tx = kv.start();
      if(!tx)
          continue;
      int64_t key = dist(rng);
      kv::create_kv_request(reinterpret_cast<uint8_t*>(&req), tx->id, key);
      tx->key = key;
      kv.send(&req, sizeof(req));
      ++t;
  }
  while(c < dur){
      cif.poll();
      rcvd = kv.recv(&resp, sizeof(resp));
      if(!rcvd)
          continue;
      kv.complete(resp.id);
      ++c;
  }
  kv.acknowledge_all();
  kv.flush();
  auto stats = kv.con->get_transport_stats();
  std::cerr << stats.rtt << ", " << stats.retransmissions
            << std::endl;
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

  auto nthreads = rte_lcore_count();
  unsigned i = 0;
  uint16_t lcore_id;
  std::vector<std::shared_ptr<message_allocator>> allocators;
  allocators.reserve(nthreads);
  RTE_LCORE_FOREACH(lcore_id) {
    allocators.emplace_back(std::make_shared<message_allocator>(
        ("mpool" + std::to_string(i)).c_str(), 16383));
    ++i;
  }
  auto ifc = iface::configure_port(0, nthreads, nthreads, allocators);
  if (!ifc)
    return -1;

  lcore_adapter adapter(nthreads, {conf.dip, conf.dport});
  adapter.dmac = conf.dmac;
  i = 0;
  RTE_LCORE_FOREACH(lcore_id) {
    auto [port, txq, rxq] = ifc->get_slice(i);
    adapter.allocator[i] = std::move(allocators[i]);
    adapter.cifs[i] = std::make_unique<client_iface>(
        port, txq, rxq, adapter.allocator[i],
        con_config{conf.sip, conf.sports[i]});
    ++i;
  }

  run(lcore_fn, &adapter);
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
