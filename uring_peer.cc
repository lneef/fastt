#include <arpa/inet.h>
#include <bits/getopt_core.h>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <kv_protocol.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <poll.h>
#include <pthread.h>
#include <random>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include <hdr/hdr_histogram.h>

#include "bench.h"
#include "uring/cpu.h"
#include "uring/iface.h"
#include <tlx/container/btree_map.hpp>
static bench::storage store;


static size_t handle_request(kv::kv_packet<kv::kv_request> *req,
                             uring::slot &slt) {
  kv::kv_packet<kv::kv_completion> *completion;
  auto it = store.find(req->payload.key);

  size_t resp_size = sizeof(*completion);
  if (it == store.end()) {
    completion = reinterpret_cast<kv::kv_packet<kv::kv_completion> *>(
        slt.tx_buffer.reserve(resp_size));
    if (!completion)
      return 0;
    completion->payload.reponse = kv::response_t::FAILURE;
    completion->payload.data_len = 0;
  } else {
    resp_size += it->second.size();
    completion = reinterpret_cast<kv::kv_packet<kv::kv_completion> *>(
        slt.tx_buffer.reserve(resp_size));
    if (!completion)
      return 0;
    completion->payload.reponse = kv::response_t::SUCCESS;
    std::memcpy(completion->payload.data, it->second.data(), it->second.size());
    completion->payload.data_len = it->second.size();
  }
  completion->id = req->id;
  completion->pt = req->pt;
  completion->payload.key = req->payload.key;
  assert(req->pt == kv::packet_t::SINGLE);
  assert(req->payload.op == kv::request_t::GET);
  return resp_size;
}

static bool request_single(uring::slot &slt, int64_t &key, std::mt19937& rng, std::uniform_int_distribution<int64_t>& dist) {
  auto *req = slt.tx_buffer.reserve(sizeof(kv::kv_packet<kv::kv_request>));
  if (!req)
    return false;
  std::memset(req, 0, sizeof(kv::kv_packet<kv::kv_request>));
  key = dist(rng);
  kv::create_kv_request(req, 0, key);
  assert(reinterpret_cast<kv::kv_packet<kv::kv_request> *>(req)->payload.op ==
         kv::request_t::GET);
  return true;
}

bool is_all_zero(const unsigned char *data, size_t size) {
  return std::all_of(data, data + size, [](unsigned char b) { return b == 0; });
}

static void parse_request(uring::slot &slt) {
  using kv_request_t = kv::kv_packet<kv::kv_request>;
  unsigned i = 0;
  unsigned resp_size = 0;
  auto size = slt.rbuffer.off;
  auto *data = slt.rbuffer.data();
  for (; i < size;) {
    auto *req = reinterpret_cast<kv_request_t *>(data + i);
    if (size - i < sizeof(kv_request_t))
      break;
    resp_size = handle_request(req, slt);
    if (!resp_size)
      break;
    i += sizeof(kv_request_t);
  }
  std::memmove(data, data + i, size - i);
  slt.rbuffer.reset(size - i);
}

template <typename F>
static unsigned parse_completion(uring::slot &slt, F &&cb) {
  using packet_t = kv::kv_packet<kv::kv_completion>;
  unsigned i = 0;
  unsigned c = 0;
  volatile auto data = slt.rbuffer.data();
  auto size = slt.rbuffer.off;
  packet_t resp;
  for (; i < size;) {
    if (size - i < sizeof(packet_t))
      break;
    std::memcpy(&resp, data + i, sizeof(packet_t));
    i += sizeof(packet_t);
    if (size - i < resp.payload.data_len) {
      i -= sizeof(packet_t);
      break;
    }

    i += resp.payload.data_len;
    cb(&resp);
    ++c;
  }
  std::memmove(data, data + i, size - i);
  slt.rbuffer.reset(size - i);
  return c;
}

static bool submit_send(uring::iface_base &iface, uring::slot &slt, int fd) {
  if (slt.tx_inflight)
    return false;
  if (slt.tx_buffer.size() == 0)
    return false;
  auto *sqe = iface.ctx->get_sqe();
  if (!sqe)
    return false;
  slt.prepare_send(slt.tx_buffer.front(), slt.tx_buffer.size(), slt.idx, fd,
                   sqe);
  assert(slt.tx_inflight);
  return true;
}

static void handle_recv(uring::iface_base &iface, uring::slot &slt,
                        unsigned idx, size_t size) {
  slt.incoming.emplace_back(idx, size);
  iface.drain_incoming(slt);
}

template <typename F>
static uint64_t process_completions(uring::client_iface &iface, F &&cb) {
  unsigned head = 0;
  unsigned c = 0;
  unsigned cnt = 0;
  int ret;
  struct io_uring_cqe *cqe;
  ret = iface.uring_submit_and_get_events();
  if (ret < 0) {
    fprintf(stderr, "submission failed %s\n", strerror(-ret));
    return ret;
  }
  uring::drain_rx_renew(&iface);
  io_uring_for_each_cqe(&iface.ctx->ring, head, cqe) {
    iface.handle_cqe(
        cqe, [&](unsigned idx, size_t size, [[maybe_unused]] unsigned sidx) {
          handle_recv(iface, iface.slt, idx, size);
        return 0;
        });
    ++cnt;
  }
  io_uring_cq_advance(&iface.ctx->ring, cnt);
  iface.drain_incoming(iface.slt);
  c += parse_completion(iface.slt, cb);
  submit_send(iface, iface.slt, iface.fd);
  return c;
}

static int client_fun_open(uint16_t id, struct sockaddr_in addr,
                           uint64_t duration, double rate) {
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<int64_t> dist(0, bench::kStoreSize);
  set_thread_affinity(pthread_self(), id);
  uring::client_iface iface{};
  struct io_uring_cqe *cqe;
  iface.setup(0);
  iface.uring_connect(&addr);
  iface.uring_submit_and_wait();
  io_uring_peek_cqe(&iface.ctx->ring, &cqe);
  if (cqe->res < 0) {
    fprintf(stderr, "Failed to connect: %s\n", strerror(-cqe->res));
    return cqe->res;
  }

  io_uring_cq_advance(&iface.ctx->ring, 1);
  iface.prepare_recv();

  std::exponential_distribution<> exp(rate);
  auto start_time = rdtsc_precise() + 1 * get_tsc_freq();
  uint64_t ticks_per_sec = get_tsc_freq();
  duration *= ticks_per_sec;
  auto ticks_per_us = ticks_per_sec / (1e6);
  auto end_time = start_time + duration;
  uint64_t next = start_time + ticks_per_sec * exp(rng);
  hdr_histogram *hist;
  std::deque<bench::req_desc_t> reqs;
  uint64_t inflight = 0;
  hdr_init(1, 500'000, 3, &hist);
  auto rx_cb = [&](kv::kv_packet<kv::kv_completion> *resp) {
    auto [t, k] = reqs.front();
    assert(resp->payload.key == k);
    --inflight;
    hdr_record_value(hist, (rdtsc() - t) / ticks_per_us);
    reqs.pop_front();
  };

  while (next < end_time) {
    if (rdtsc() < next) {
      process_completions(iface, rx_cb);
      continue;
    }

    int64_t key;
    while (!request_single(iface.slt, key, rng, dist))
      process_completions(iface, rx_cb);
    ++inflight;
    reqs.emplace_back(next, key);
    next += ticks_per_sec * exp(rng);
  }
  while (inflight > 0)
    process_completions(iface, rx_cb);

  FILE *f = fopen("latency.uring.hgrm", "w");
  hdr_percentiles_print(hist, f, 5, 1.0, CLASSIC);
  fclose(f);
  return 0;
}

static int server_fun(uint16_t id, unsigned sz, int port_arg, in_addr_t addr) {
  set_thread_affinity(pthread_self(), id);
  int ret;
  bench::prepare(store, sz);
  struct io_uring_cqe *cqe;
  uring::server_iface iface;
  iface.setup(port_arg);
  unsigned head = 0;
  ret = iface.prepare_listen(addr);
  if (ret) {
    fprintf(stderr, "Set listen failed: %s\n", strerror(-ret));
    return ret;
  }

  ret = iface.uring_prepare_accept();
  if (ret) {
    fprintf(stderr, "Prepare accept failed: %s\n", strerror(-ret));
    return ret;
  }
  while (true) {
    ret = iface.uring_submit_and_wait();
    if (ret < 0) {
      fprintf(stderr, "submission failed %s\n", strerror(-ret));
      return ret;
    }
    uring::drain_rx_renew(&iface);
    unsigned cnt = 0;
    io_uring_for_each_cqe(&iface.ctx->ring, head, cqe) {
      iface.handle_cqe(cqe, [&](unsigned idx, size_t size, unsigned sidx) {
        auto &slt = iface.slot_at(sidx);
        handle_recv(iface, slt, idx, size);
        return 0;
      });
      ++cnt;
    }
    io_uring_cq_advance(&iface.ctx->ring, cnt);

    for (auto &slt : iface.active) {
      iface.drain_incoming(slt);
      parse_request(slt);
      submit_send(iface, slt, iface.clients[slt.idx]);
    }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  uint16_t port_arg = 0;
  int opt, nt = 1;
  bool is_client = false;
  bool did_init_addr = false;
  uint64_t duration = 5;
  double rate = 100000;
  size_t sz = 8;
  struct in_addr ip_addr;
  std::vector<std::thread> threads;

  while ((opt = getopt(argc, argv, "p:ca:t:d:r:os:P")) != -1) {
    switch (opt) {
    case 'p':
      port_arg = std::atoi(optarg);
      break;
    case 'c':
      is_client = true;
      break;
    case 'a':
      inet_aton(optarg, &ip_addr);
      did_init_addr = true;
      break;
    case 't':
      nt = std::atoi(optarg);
      break;
    case 'd':
      duration = std::atol(optarg);
      break;
    case 'r':
      rate = std::stod(optarg);
      break;

    case 's':
      sz = std::atol(optarg);
      break;

    default:
      fprintf(stderr,
              "Usage: %s [-p port] "
              "[-b log2(BufferSize)] [-6] [-v]\n",
              argv[0]);
      exit(-1);
    }
  }
  threads.reserve(nt);
  uint16_t pidx = 0;
  for (uint16_t i = 0; i < nt; ++i) {
    if (is_client) {
      threads.emplace_back(client_fun_open, i,
                           sockaddr_in{.sin_family = AF_INET,
                                       .sin_port = htons(port_arg),
                                       .sin_addr = {ip_addr},
                                       .sin_zero = {}},
                           duration, rate);

    } else {
      threads.emplace_back(server_fun, i, sz, port_arg + pidx++,
                           did_init_addr ? ip_addr.s_addr : INADDR_ANY);
    }
  }

  for (auto &t : threads)
    t.join();
  return 0;
}
