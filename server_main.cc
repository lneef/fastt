#include "bench.h"
#include "connection.h"
#include "dpdk/allocator.h"
#include "iface.h"
#include "kv.h"
#include "kv_protocol.h"
#include "server.h"
#include "sgl.h"
#include "slab_allocator.h"
#include "task/async.h"
#include "task/task.h"
#include <arpa/inet.h>
#include <atomic>
#include <bits/getopt_core.h>
#include <cstdint>
#include <cstring>
#include <getopt.h>
#include <memory>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <signal.h>
#include <sys/types.h>
#include <utility>
#include <tlx/container/btree_map.hpp>

struct netconfig {
  uint32_t sip;
};

struct lcore_server_adapter {
  std::unique_ptr<server_iface> iface;
  std::shared_ptr<dpdk_allocator> allocator;
};

static bench::storage store;
static unsigned len = 8;
static unsigned store_size = bench::kStoreSize;

template <typename T>
static T *alloc_or_get(sgl &rsgl, slab_allocator &alloc, uint32_t len,
                       batch &btch) {
  auto *completion = btch.next<T>(len);
  if (!completion) {
    rsgl.add_segment_safe(std::move(btch).release());
    auto seg = alloc.alloc_default_safe(0);
    btch = batch(seg);
    completion = btch.next<T>(len);
  }
  return completion;
}

#ifdef NBATCH
static void serve(sgl &resp_sgl, slab_allocator &alloc,
                  kv::kv_packet<kv::kv_request> *packet) {
  kv::kv_packet<kv::kv_completion> *completion;
  auto key = packet->payload.key;
  auto it = store.find(key);
  if (it == store.end()) {
    auto seg = alloc.alloc_default_safe(sizeof(*completion));
    completion = seg->data<kv::kv_packet<kv::kv_completion>>();
    completion->payload.reponse = kv::response_t::FAILURE;
    completion->payload.data_len = 0;
    resp_sgl.add_segment_safe(std::move(seg));
  } else {
    auto seg =
        alloc.alloc_default_safe(sizeof(*completion) + it->second.size());
    completion = seg->data<kv::kv_packet<kv::kv_completion>>();
    completion->payload.reponse = kv::response_t::SUCCESS;
    std::memcpy(completion->payload.data, it->second.data(), it->second.size());
    completion->payload.data_len = it->second.size();
    resp_sgl.add_segment_safe(std::move(seg));
  }
  completion->id = packet->id;
  completion->pt = packet->pt;
  completion->payload.key = packet->payload.key;
}
#else

static void serve(batch &btch, sgl &resp_sgl, slab_allocator &alloc,
                  kv::kv_packet<kv::kv_request> *packet) {
  kv::kv_packet<kv::kv_completion> *completion;
  auto key = packet->payload.key;
  auto it = store.find(key);
  if (it == store.end()) {
    completion = alloc_or_get<kv::kv_packet<kv::kv_completion>>(
        resp_sgl, alloc, sizeof(*completion), btch);
    completion->payload.reponse = kv::response_t::FAILURE;
    completion->payload.data_len = 0;
  } else {
    completion = alloc_or_get<kv::kv_packet<kv::kv_completion>>(
        resp_sgl, alloc, sizeof(*completion) + it->second.size(), btch);
    completion->payload.reponse = kv::response_t::SUCCESS;
    std::memcpy(completion->payload.data, it->second.data(), it->second.size());
    completion->payload.data_len = it->second.size();
  }
  completion->id = packet->id;
  completion->pt = packet->pt;
  completion->payload.key = packet->payload.key;
  btch.finalize(completion->payload.data_len +
                sizeof(kv::kv_packet<kv::kv_completion>));
}
#endif

static netconfig parse_cmdline(int argc, char *argv[]) {
  int opt, option_index;
  netconfig conf{};
  static const struct option long_options[] = {
      {"sip", required_argument, 0, 0},
      {"len", required_argument, 0, 0},
      {"size", required_argument, 0, 0},
      {0, 0, 0, 0}};
  while ((opt = getopt_long(argc, argv, "", long_options, &option_index)) !=
         -1) {
    switch (option_index) {
    case 0:
      conf.sip = inet_addr(optarg);
      break;
    case 1:
      len = atoi(optarg);
      break;
    case 2:
      store_size = std::stol(optarg);
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

#ifdef NBATCH
int lcore_server_fun(void *arg) {
  auto myid = rte_lcore_index(rte_lcore_id());
  auto &adapters = *static_cast<std::vector<lcore_server_adapter> *>(arg);
  auto *server = adapters[myid].iface.get();
  server->register_service(
      2, [](server_iface &iface, connection &con) -> concurrency::task {
        sgl ssgl;
        auto &slab = *iface.get_alloc();
        while (true) {
          sgl rsgl{};
          auto sz = co_await recv(iface.get_scheduler(), con, rsgl);
          if (sz == 0) 
            co_return;
          
          assert(ssgl.empty());
          for (auto &seg : rsgl) {
            assert(seg.data_len == sizeof(kv::kv_packet<kv::kv_request>));
            serve(ssgl, slab, seg.data<kv::kv_packet<kv::kv_request>>());
          }
          ssize_t to_send = ssgl.size;
          auto sent =
              co_await send(iface.get_scheduler(), con, std::move(ssgl));
          if (sent == 0)
            co_return;
          assert(sent == to_send);
        }
      });

  while (!terminate)
    server->run();
  server->complete();

  return 0;
}
#else
int lcore_server_fun(void *arg) {
  auto myid = rte_lcore_index(rte_lcore_id());
  auto &adapters = *static_cast<std::vector<lcore_server_adapter> *>(arg);
  auto *server = adapters[myid].iface.get();
  server->register_service(
      2, [](server_iface &iface, connection &con) -> concurrency::task {
        sgl ssgl;
        auto &slab = *iface.get_alloc();
        while (true) {
          sgl rsgl{};
          auto sz = co_await recv(iface.get_scheduler(), con, rsgl);
          if (sz == 0)
            co_return;
          auto seg = slab.alloc_default_safe(0);
          batch btch(seg);
          assert(ssgl.empty());
          for (auto &seg : rsgl) {
            parse_mbuf<kv::kv_packet<kv::kv_request>>(
                seg, [&](kv::kv_packet<kv::kv_request> *req) {
                  serve(btch, ssgl, slab, req);
                  return sizeof(kv::kv_packet<kv::kv_request>);
                });
          }
          ssgl.add_segment_safe(std::move(btch).release());
          ssize_t to_send = ssgl.size;
          auto sent =
              co_await send(iface.get_scheduler(), con, std::move(ssgl));
          if (sent == 0)
            co_return;
          assert(sent == to_send);
        }
      });

  while (!terminate)
    server->run();
  server->complete();

  return 0;
}
#endif

int run(netconfig &conf) {
  if (fastt::init())
    return -1;

  auto nthreads = rte_lcore_count();
  unsigned i = 0;
  uint16_t lcore_id;
  std::vector<std::shared_ptr<dpdk_allocator>> allocators;
  std::vector<uint16_t> lcore_ids;
  allocators.reserve(nthreads);
  lcore_ids.reserve(nthreads);
  RTE_LCORE_FOREACH(lcore_id) {
    allocators.emplace_back(
        dpdk_allocator::create(("mpool" + std::to_string(i)).c_str(), 4095));
    lcore_ids.push_back(lcore_id);
    ++i;
  }
  auto ifc =
      iface::configure_port(0, nthreads, nthreads, allocators, lcore_ids);
  if (!ifc)
    return -1;
  bench::prepare(store, len, store_size);
  printf("Setup complete\n");

  std::vector<lcore_server_adapter> adapters(nthreads);
  i = 0;
  RTE_LCORE_FOREACH(lcore_id) {
    auto &adapter = adapters[i];
    auto [port, txq, rxq] = ifc->get_slice(i);
    adapter.allocator = std::move(allocators[i]);
    adapter.iface = std::make_unique<server_iface>(
        port, txq, rxq, conf.sip, adapter.allocator, rte_lcore_count());
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
