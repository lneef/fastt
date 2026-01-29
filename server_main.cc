#include "connection.h"
#include "iface.h"
#include "kv.h"
#include "message.h"
#include "server.h"
#include "transport/slot.h"
#include <arpa/inet.h>
#include <bits/getopt_core.h>
#include <cstdint>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <ostream>
#include <random>
#include <ranges>
#include <rte_ether.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <unordered_map>
#include <utility>
#include <signal.h>
#include <format>

struct netconfig {
  rte_ether_addr dmac;
  uint32_t sip, dip;
  uint16_t sport, dport;
};

static std::random_device dev;
static std::mt19937 rng(dev());
static std::uniform_int_distribution<std::mt19937::result_type> dist(INT64_MIN,
                                                                     INT64_MAX);
static constexpr uint32_t kStoreSize = 1024;
static std::unordered_map<int64_t, int64_t> store(kStoreSize);

static void prepare() {
  for (auto [k, v] :
       std::ranges::views::iota(0, 1000) | std::views::transform([&](int) {
         return std::make_pair(dist(rng), dist(rng));
       })) {
    store[k] = v;
  }
}

static message *serve(message_allocator *allocator,
                      kv_packet<kv_request> *packet) {
  auto key = packet->payload.key;
  auto it = store.find(key);

  message *msg = allocator->alloc_message(sizeof(kv_packet<kv_completion>));
  auto *completion = rte_pktmbuf_mtod(msg, kv_packet<kv_completion> *);
  completion->id = packet->id;
  completion->pt = packet->pt;
  if (it == store.end()) {
    completion->payload.reponse = response_t::FAILURE;
    completion->payload.val = 0;
  } else {
    completion->payload.reponse = response_t::SUCCESS;
    completion->payload.val = it->second;
  }
  return msg;
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

static volatile int terminate = 0;

static void handler(int sig) {
(void)sig;
terminate = 1;
}

int run(netconfig &conf) {
  prepare();
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
    server.poll([&](transaction_slot &slot) {
      auto *msg = slot.rx_if.read();
      auto *resp = serve(allocator.get(),
                         rte_pktmbuf_mtod(msg, kv_packet<kv_request> *));
      slot.tx_if.send(resp, true);
      if (!slot.has_outstanding_messages())
        slot.finish();
      message_allocator::deallocate(msg);
    });
    server.complete();
  }

  auto stats = server.get_stats();
  std::cout << std::format("total: {0}, no: {1}\n", stats.total_rx_polled, stats.no_rx) << std::endl;
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
