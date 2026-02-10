/* SPDX-License-Identifier: MIT */

#include <arpa/inet.h>
#include <bit>
#include <bits/getopt_core.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <kv_protocol.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <random>
#include <ranges>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "uring/iface.h"
#include "uring/tcp.h"
#include <tlx/container/btree_map.hpp>

static constexpr uint32_t kDefaultTXN = 1e6;
static constexpr uint16_t kDefaultSQBatch = 8;

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
  assert(store.size() == kStoreSize);
}

struct slot_storage{
    std::deque<unsigned> free_slots;
    slot_storage(unsigned n){
        for(unsigned i = 0; i < n; ++i)
            free_slots.push_back(i);
    }
};


unsigned seen = 0;
static int handle_request(kv::kv_packet<kv::kv_request> *req,
                          kv::kv_packet<kv::kv_completion> *completion) {
  auto key = req->payload.key;
  auto it = store.find(key);
  completion->id = req->id;
  completion->pt = req->pt;
  completion->payload.key = req->payload.key;
  if (it == store.end()) {
    completion->payload.reponse = kv::response_t::FAILURE;
    completion->payload.val = 0;
  } else {
    completion->payload.reponse = kv::response_t::SUCCESS;
    completion->payload.val = it->second;
  }
  ++seen;
  return 0;
}

static uint64_t request_batch(uring::client_iface *st, slot_storage& slt_strge, uint64_t t, uint8_t bs) {
  for (auto i = 0u; i < std::min<unsigned>(bs, kDefaultTXN - t); ++i) {
    if(!slt_strge.free_slots.size())
        break;
    uint8_t *buf = static_cast<uint8_t *>(st->pool.alloc());
    if (!buf)
      break;
    auto *sqe = st->ctx->get_sqe();
    if (!sqe) {
      st->pool.free(buf);
      break;
    }
    auto slt_id = slt_strge.free_slots.front();
    slt_strge.free_slots.pop_front();
    int64_t key = dist(rng);
    kv::create_kv_request(buf, slt_id, key);
    ++t;
    st->prepare_send(buf, sizeof(kv::kv_packet<kv::kv_request>), std::bit_cast<uint64_t>(buf),  st->fd, sqe);
  }
  return t;
}

unsigned prsd = 0;
static std::pair<size_t, int> parse_request(uring::server_iface &iface,
                                            uint8_t *data, size_t size,
                                            unsigned idx) {
  using kv_request_t = kv::kv_packet<kv::kv_request>;
  using kv_response_t = kv::kv_packet<kv::kv_completion>;
  int ret = 0;
  unsigned i = 0;
  struct io_uring_sqe *sqe = nullptr;
  unsigned char *sbuf = nullptr;
  unsigned resp_off = 0;
  for (; i < size;) {
    auto *req = reinterpret_cast<kv_request_t *>(data + i);
    if (size - i < sizeof(kv_request_t))
      break;

    if (!sbuf) {
      sbuf = static_cast<uint8_t *>(iface.pool.alloc());
      if (!sbuf) {
        assert(0);
        ret = -1;
        goto end;
      }
    }

    if (!sqe) {
      sqe = iface.ctx->get_sqe();
      if (!sqe) {
        assert(0);
        iface.pool.free(sbuf);
        ret = -1;
        goto end;
      }
    }

    handle_request(req, new (sbuf + resp_off) kv_response_t);
    resp_off += sizeof(kv_response_t);
    if (iface.pool.kElemSize - resp_off < sizeof(kv_response_t)) {
      iface.prepare_send(sbuf, resp_off, std::bit_cast<uint64_t>(sbuf),
                         iface.clients[idx], sqe);
      resp_off = 0;
      sbuf = nullptr;
      sqe = nullptr;
    }

    i += sizeof(kv_request_t);
  }
end:
  if(sqe)
    iface.prepare_send(sbuf, resp_off, std::bit_cast<uint64_t>(sbuf),
                         iface.clients[idx], sqe);
  prsd += i;
  std::memmove(data, data + i, size - i);
  return {size - i, ret};
}

static std::pair<size_t, unsigned> parse_completion(slot_storage& slt_strge, uint8_t *data,
                                                    size_t size) {
  using packet_t = kv::kv_packet<kv::kv_completion>;
  unsigned i = 0;
  unsigned c = 0;
  for (; i < size;) {
    if (size - i < sizeof(packet_t))
      break;

    auto *resp = reinterpret_cast<packet_t *>(data + i);
    slt_strge.free_slots.push_back(resp->id);
    ++c;
    i += sizeof(packet_t);
  }
  prsd += i;
  std::memmove(data, data + i, size - i);
  return {size - i, c};
}

static uint64_t process_completions(uring::client_iface *st, slot_storage& slt_strge) {
  unsigned head = 0;
  unsigned c = 0;
  unsigned cnt = 0;
  int ret;
  struct io_uring_cqe *cqe;
  ret = st->uring_submit_and_wait(&cqe);
  if (ret == -ETIME)
    return 0;
  if (ret < 0) {
    fprintf(stderr, "submission failed %s\n", strerror(-ret));
    return ret;
  }
  io_uring_for_each_cqe(&st->ctx->ring, head, cqe) {
    st->handle_cqe(cqe, [&](void *buf, size_t size, unsigned sidx) {
      (void)sidx;
      st->rbuffer.cpy(buf, size);
      auto [off, nc] =
          parse_completion(slt_strge, st->rbuffer.buffer.data(), st->rbuffer.off);
      st->rbuffer.reset(off);    

      c += nc;
      return 0;
    });
    ++cnt;
  }
  io_uring_cq_advance(&st->ctx->ring, cnt);
  return c;
}

static int client_fun(struct sockaddr_in *addr) {
  static constexpr uint16_t kNumSlots = 64;  
  uring::client_iface iface{};
  struct io_uring_cqe *cqe;
  iface.setup(0);
  uint64_t t = 0, c = 0;
  iface.uring_connect(addr);
  iface.uring_submit_and_wait();
  io_uring_peek_cqe(&iface.ctx->ring, &cqe);
  io_uring_cq_advance(&iface.ctx->ring, 1);
  if (cqe->res < 0) {
    fprintf(stderr, "Failed to connect: %d\n", cqe->res);
    return cqe->res;
  }
  slot_storage slt_strge(kNumSlots);
  iface.prepare_recv();
  auto start = std::chrono::steady_clock::now();
  while (kDefaultTXN > t) {
    c += process_completions(&iface, slt_strge);
    t = request_batch(&iface, slt_strge, t, kDefaultSQBatch);
  }

  while (c < kDefaultTXN) {
    c += process_completions(&iface, slt_strge);
  }
  struct tcp_info info;
  uring::tcp::get_tcp_stats(iface.fd, &info);
  uring::tcp::print_tcp_info(stdout, &info);
  auto end = std::chrono::steady_clock::now();
  printf("%f\n", std::chrono::duration<double, std::micro>(end - start).count());
  return 0;
}

static int server_fun(int port_arg, in_addr_t addr) {
  int ret;
  prepare();
  struct io_uring_cqe *cqe;
  uring::server_iface iface{};
  iface.setup(port_arg);
  unsigned head = 0;
  ret = iface.uring_prepare_listen(addr);
  if (ret) {
    fprintf(stderr, "Set listen failed: %s\n", strerror(-ret));
    return ret;
  }

  ret = iface.uring_prepare_accept();
  if (ret) {
    fprintf(stderr, "Prepare accept failed: %s\n", strerror(-ret));
    return ret;
  }
  unsigned rx = 0;
  while (true) {
    ret = iface.uring_submit_and_wait(&cqe);
    if (ret < 0 && ret != -ETIME) {
      fprintf(stderr, "submission failed %s\n", strerror(-ret));
      return ret;
    }
    unsigned cnt = 0;
    io_uring_for_each_cqe(&iface.ctx->ring, head, cqe) {
      ret = iface.handle_cqe(cqe, [&](void *buf, size_t size, unsigned sidx) {
        auto &slt = iface.connection_state(sidx);
        slt.rbuffer.cpy(buf, size);
        rx += size;
        auto [off, ret] = parse_request(iface, slt.rbuffer.data(),
                                        slt.rbuffer.off, sidx);
        slt.rbuffer.reset(off);
        return ret;
      });
      ++cnt;
      if (ret)
        break;
    }
    io_uring_cq_advance(&iface.ctx->ring, cnt);

    for (auto& slt: iface.active) {
      auto [off, ret] = parse_request(iface, slt.rbuffer.buffer.data(),
                                      slt.rbuffer.off, slt.idx);
      slt.rbuffer.reset(off);
    }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  uint16_t port_arg = 0;
  int opt, ret = 0;
  bool is_client = false;
  bool did_init_addr = false;
  struct sockaddr_in addr;

  while ((opt = getopt(argc, argv, "p:ca:")) != -1) {
    switch (opt) {
    case 'p':
      port_arg = std::atoi(optarg);
      break;
    case 'c':
      is_client = true;
      break;
    case 'a':
      inet_aton(optarg, &addr.sin_addr);
      did_init_addr = true;
      break;
    default:
      fprintf(stderr,
              "Usage: %s [-p port] "
              "[-b log2(BufferSize)] [-6] [-v]\n",
              argv[0]);
      exit(-1);
    }
  }
  addr.sin_port = htons(port_arg);
  addr.sin_family = AF_INET;
  if (is_client)
    ret = client_fun(&addr);
  else
    ret = server_fun(port_arg, did_init_addr ? addr.sin_addr.s_addr : INADDR_ANY);
  return ret;
}
