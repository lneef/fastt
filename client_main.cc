#include "bench.h"
#include "client.h"
#include "connection.h"
#include "dpdk/allocator.h"
#include "iface.h"
#include "kv.h"
#include "kv_protocol.h"
#include "sgl.h"
#include "slab_allocator.h"
#include "util.h"
#include <arpa/inet.h>
#include <atomic>
#include <barrier>
#include <bits/getopt_core.h>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <generic/rte_cycles.h>
#include <getopt.h>
#include <hdr/hdr_histogram.h>
#include <hdr/hdr_histogram_log.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <ranges>
#include <rte_lcore.h>
#include <sys/types.h>
#include <vector>

std::mutex mtx;
alignas(RTE_CACHE_LINE_MIN_SIZE) std::atomic<double> lat = 0;

static std::atomic<unsigned> slot_wnd = 8;
static size_t keySpace = bench::kStoreSize;

struct netconfig {
  rte_ether_addr dmac;
  uint32_t sip, dip;
  uint16_t dport;
  uint64_t duration;
  double rate;
  bool open = false;
  std::vector<uint16_t> sports;
};

struct lcore_adapter {
  std::vector<std::tuple<uint16_t, uint16_t, uint16_t>> cifs;
  std::vector<std::shared_ptr<dpdk_allocator>> allocator;
  con_config cfg;
  netconfig nef_cfg;
  rte_ether_addr dmac;
  uint64_t duration;
  double rate;
  unsigned nthreads = 0;

  std::barrier<> barrier;

  lcore_adapter(std::size_t n, con_config cfg, netconfig &nef_cfg)
      : allocator(n, nullptr), cfg(cfg), nef_cfg(nef_cfg), nthreads(n), barrier(n) {
          cifs.resize(n);
      }
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
      {"duration", required_argument, 0, 0},
      {"rate", required_argument, 0, 0},
      {"open", no_argument, 0, 0},
      {"wnd", required_argument, 0, 0},
      {"kspace", required_argument, 0, 0},
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
    case 5:
      conf.duration = atoll(optarg) * rte_get_timer_hz();
      break;
    case 6:
      conf.rate = std::stod(optarg);
      break;
    case 7:
      conf.open = true;
      break;
    case 8:
      slot_wnd = atoi(optarg);
      break;
    case 9:
      keySpace = std::stol(optarg);
      break;
    default:
      break;
    }
  }
  return conf;
}

static int lcore_closed_fn(void *arg) {
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<int64_t> dist(0, keySpace);
  auto *adapter = static_cast<lcore_adapter *>(arg);
  auto me = rte_lcore_index(rte_lcore_id());
  assert(adapter->cifs.size() == adapter->nthreads);
  auto [port, txq, rxq] = adapter->cifs[me];
  auto cif = std::make_unique<client_iface>(
      port, txq, rxq, adapter->allocator[me],
      con_config{adapter->nef_cfg.sip, adapter->nef_cfg.sports[me]},
      adapter->nthreads);

  adapter->barrier.arrive_and_wait();
  kv_proxy kv(cif.get(), slot_wnd);
  kv.connect(adapter->cfg, adapter->dmac);
  auto *sb = cif->manager.get_allocator();
  hdr_histogram *hist;
  hdr_init(1, 500'000, 3, &hist);

  uint64_t rpcs_cnt = 0;
  uint64_t rpcs_finished = 0;
  uint64_t inflight = 0;

  auto rx_fn = [&] {
    for (;;) {
      sgl rsgl;
      auto rcvd = kv.recv(rsgl);
      if (rcvd <= 0)
        break;
      auto now = rte_get_timer_cycles();
      for (auto &seg : rsgl) {
        parse_mbuf<kv::kv_packet<kv::kv_completion>>(
            seg, [&](kv::kv_packet<kv::kv_completion> *resp) {
              assert(resp->payload.key == kv[resp->id].key);
              kv.complete(resp->id);
              hdr_record_value(hist, (now - kv[resp->id].ts) / get_ticks_us());
              ++rpcs_cnt;
              --inflight;
              return resp->payload.data_len +
                     sizeof(kv::kv_packet<kv::kv_completion>);
            });
      }
    }
  };

  auto start = rte_get_timer_cycles();
  auto end = start + adapter->duration;

  while (start < end) {
    cif->poll();
    rx_fn();
    auto *tx = kv.start();
    if (!tx)
      continue;
    int64_t key = dist(rng);
    auto *m = sb->alloc_default(sizeof(kv::kv_packet<kv::kv_request>));
    kv::create_kv_request(m->data<uint8_t>(), tx->id, key);
    tx->key = key;
    tx->ts = rte_get_timer_cycles();
    sgl ssgl;
    ssgl.add_segment_safe(mbuf_take_owner_ship(m));
    auto sent = 0u;
    while (sent < sizeof(kv::kv_packet<kv::kv_request>)) {
      auto retval = kv.send(ssgl);
      if (retval == -EAGAIN) {
        cif->poll();
        rx_fn();
      } else
        sent += retval;
    }
    assert(sent == sizeof(kv::kv_packet<kv::kv_request>));
    ++inflight;
    start = rte_get_timer_cycles();
  }
  rpcs_finished = rpcs_cnt;
  while (inflight) {
    cif->poll();
    rx_fn();
  }

  kv.close();
  std::lock_guard lg(mtx);
  std::cout << static_cast<double>(rpcs_finished) /
                   (static_cast<double>(adapter->duration) / rte_get_timer_hz())
            << std::endl;
  std::cout << hdr_value_at_percentile(hist, 99.0) << std::endl;
  return 0;
}

static int lcore_open_fn(void *arg) {
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<int64_t> dist(0, keySpace);
  auto *adapter = static_cast<lcore_adapter *>(arg);
  auto me = rte_lcore_index(rte_lcore_id());
  auto [port, txq, rxq] = adapter->cifs[me];
  auto cif = std::make_unique<client_iface>(
      port, txq, rxq, adapter->allocator[me],
      con_config{adapter->nef_cfg.sip, adapter->nef_cfg.sports[me]},
      adapter->nthreads);

  adapter->barrier.arrive_and_wait();
  kv_proxy kv(cif.get(), slot_wnd);
  kv.connect(adapter->cfg, adapter->dmac);
  auto *sb = cif->manager.get_allocator();
  size_t rpcs = 0;
  std::exponential_distribution<double> exp(adapter->rate);
  auto start_time = rte_get_timer_cycles() + 10 * rte_get_timer_hz();
  auto ticks_per_sec = rte_get_timer_hz();
  auto end_time = start_time + adapter->duration;
  auto next = start_time + ticks_per_sec * exp(rng);
  hdr_histogram *hist;
  hdr_init(1, 500'000, 3, &hist);

  std::deque<bench::req_desc_t> reqs;
  uint64_t inflight = 0;
  auto rx_fn = [&](kv_proxy &pry) {
    for (;;) {
      sgl rsgl{};
      auto rcvd = pry.recv(rsgl);
      if (rcvd <= 0)
        break;
      for (auto &seg : rsgl) {
        parse_mbuf<kv::kv_packet<kv::kv_completion>>(
            seg, [&](kv::kv_packet<kv::kv_completion> *resp) {
              auto [t, k] = reqs.front();
              ensure(resp->payload.key == k);
              hdr_record_value(hist,
                               (rte_get_timer_cycles() - t) / get_ticks_us());
              reqs.pop_front();
              --inflight;
              ++rpcs;
              return resp->payload.data_len +
                     sizeof(kv::kv_packet<kv::kv_completion>);
            });
      }
    }
  };
  while (next < end_time) {
    cif->poll();
    rx_fn(kv);
    if (rte_get_timer_cycles() < next)
      continue;
    int64_t key = dist(rng);
    reqs.emplace_back(next, key);
    auto *m = sb->alloc_default(sizeof(kv::kv_packet<kv::kv_request>));
    kv::create_kv_request(m->data<uint8_t>(), 0, key);
    sgl ssgl;
    ssgl.add_segment_safe(mbuf_take_owner_ship(m));
    auto sent = 0u;
    while (sent < sizeof(kv::kv_packet<kv::kv_request>)) {
      auto retval = kv.send(ssgl);
      if (retval == -EAGAIN) {
        cif->poll();
        rx_fn(kv);
      } else
        sent += retval;
    }
    ++inflight;
    next += exp(rng) * rte_get_timer_hz();
  }
  while (inflight > 0) {
    cif->poll();
    rx_fn(kv);
  }

  kv.close();

  std::lock_guard lg(mtx);
  std::cout << hdr_value_at_percentile(hist, 99.0) << std::endl;
  return 0;
}

static void run(lcore_function_t *f, void *args) {
  rte_eal_mp_remote_launch(f, args, CALL_MAIN);
  rte_eal_mp_wait_lcore();
}

[[gnu::noinline]] int run(netconfig &conf) {
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
    lcore_ids.emplace_back(lcore_id);
    ++i;
  }
  auto ifc =
      iface::configure_port(0, nthreads, nthreads, allocators, lcore_ids);
  if (!ifc)
    return -1;

  lcore_adapter adapter(nthreads, {conf.dip, conf.dport}, conf);
  adapter.dmac = conf.dmac;
  adapter.duration = conf.duration;
  adapter.rate = conf.rate;
  i = 0;
  RTE_LCORE_FOREACH(lcore_id) {
    auto [port, txq, rxq] = ifc->get_slice(i);
    adapter.allocator[i] = std::move(allocators[i]);
    adapter.cifs.emplace_back(port, txq, rxq);
    ++i;
  }
  if (conf.open)
    run(lcore_open_fn, &adapter);
  else
    run(lcore_closed_fn, &adapter);
#ifdef PRINT_STATS
  {
    auto n = rte_eth_xstats_get_names(0, nullptr, 0);
    std::vector<rte_eth_xstat_name> names(n);
    std::vector<rte_eth_xstat> xstats(n);
    rte_eth_xstats_get_names(0, names.data(), n);
    rte_eth_xstats_get(0, xstats.data(), n);
    for (auto &xstat : xstats)
      printf("%s: %lu\n", names[xstat.id].name, xstat.value);
  }
#endif
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
