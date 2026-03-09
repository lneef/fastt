#include "connection.h"
#include "dpdk/allocator.h"
#include "iface.h"
#include "kv_protocol.h"
#include "server.h"
#include "task/async.h"
#include "task/task.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <getopt.h>
#include <memory>
#include <random>
#include <ranges>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <signal.h>
#include <utility>

#include <tlx/container/btree_map.hpp>

struct netconfig {
  rte_ether_addr dmac;
  uint32_t sip, dip;
  uint16_t sport, dport;
};

struct lcore_server_adapter {
  std::unique_ptr<server_iface> iface;
  std::shared_ptr<dpdk_allocator> allocator;
};

static std::random_device dev;
static std::mt19937 rng(dev());
static std::uniform_int_distribution<int64_t> dist(INT64_MIN, INT64_MAX);
static constexpr uint32_t kStoreSize = 1024 * 1024;
static tlx::btree_map<int64_t, int64_t> store;

static void prepare() {
  uint32_t size = kStoreSize;
  for (auto [k, v] :
       std::ranges::views::iota(0u, size) | std::views::transform([&](int) {
         return std::make_pair(dist(rng), dist(rng));
       })) {
    store[k] = v;
  }
}

static void serve(kv::kv_packet<kv::kv_completion> *completion,
                  kv::kv_packet<kv::kv_request> *packet) {
  auto key = packet->payload.key;
  auto it = store.find(key);

  completion->id = packet->id;
  completion->pt = packet->pt;
  completion->payload.key = packet->payload.key;
  if (it == store.end()) {
    completion->payload.reponse = kv::response_t::FAILURE;
    completion->payload.val = 0;
  } else {
    completion->payload.reponse = kv::response_t::SUCCESS;
    completion->payload.val = it->second;
  }
}

static netconfig parse_cmdline(int argc, char *argv[]) {
  int opt, option_index;
  netconfig conf;
  static const struct option long_options[] = {{"sip", required_argument, 0, 0},
                                               {0, 0, 0, 0}};
  while ((opt = getopt_long(argc, argv, "", long_options, &option_index)) !=
         -1) {
    switch (option_index) {
    case 0:
      conf.sip = inet_addr(optarg);
      break;
    }
  }
  return conf;
}

static std::atomic<int> terminate = 0;

static void handler(int sig) {
  (void)sig;
  terminate = 1;
}

int lcore_server_fun(void *arg) {
  auto myid = rte_lcore_index(rte_lcore_id());
  auto &adapters = *static_cast<std::vector<lcore_server_adapter> *>(arg);
  auto *server = adapters[myid].iface.get();
  auto *slab = server->get_alloc();
  server->register_service(
      2,
      [&](concurrency::scheduler &schdlr,
          connection &con) -> concurrency::task {
        sgl ssgl;
        sgl rsgl;
        while (true) {
          auto sz = co_await recv(schdlr, con, rsgl);
          if (sz == 0) {
            co_return;
          }
          assert(sz == sizeof(kv::kv_packet<kv::kv_request>));
          auto pkt_ptr = slab->alloc_default_safe(
              sizeof(kv::kv_packet<kv::kv_completion>));
          serve(pkt_ptr->data<kv::kv_packet<kv::kv_completion>>(),
                rsgl.head->data<kv::kv_packet<kv::kv_request>>());
          ssgl.add_segment_safe(std::move(pkt_ptr));
          auto sent = co_await send(schdlr, con, std::move(ssgl));
          if (sent == 0) {
            co_return;
          }
          assert(sent == sizeof(kv::kv_packet<kv::kv_completion>));
        }
      });

  while (!terminate)
    server->run();
  server->complete();

  return 0;
}

int run(netconfig &conf) {
  prepare();
  if (fastt::init())
    return -1;

  auto nthreads = rte_lcore_count();
  unsigned i = 0;
  uint16_t lcore_id;
  std::vector<std::shared_ptr<dpdk_allocator>> allocators;
  allocators.reserve(nthreads);
  RTE_LCORE_FOREACH(lcore_id) {
     allocators.emplace_back(dpdk_allocator::create(
        ("mpool" + std::to_string(i)).c_str(), 4095)); 
    ++i;
  }
  auto ifc = iface::configure_port(0, nthreads, nthreads, allocators);
  if (!ifc)
    return -1;

  std::vector<lcore_server_adapter> adapters(nthreads);
  i = 0;
  RTE_LCORE_FOREACH(lcore_id) {
    auto &adapter = adapters[i];
    auto [port, txq, rxq] = ifc->get_slice(i);
    adapter.allocator = std::move(allocators[i]);
    adapter.iface = std::make_unique<server_iface>(
        port, txq, rxq, con_config{conf.sip, conf.sport}, adapter.allocator,
        rte_lcore_count());
    ++i;
  }

  rte_eal_mp_remote_launch(lcore_server_fun, &adapters, CALL_MAIN);
  rte_eal_mp_wait_lcore();
  ifc->stop();
  return 0;
}

int main(int argc, char *argv[]) {
  struct sigaction sa = {};
  sa.sa_handler = handler;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  int dpdk_argc = rte_eal_init(argc, argv);
  auto conf = parse_cmdline(argc - dpdk_argc, argv + dpdk_argc);
  run(conf);
  rte_eal_cleanup();
  return 0;
}
