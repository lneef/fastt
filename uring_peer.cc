/* SPDX-License-Identifier: MIT */

#include <arpa/inet.h>
#include <bits/getopt_core.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <kv_protocol.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
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
#include <liburing.h>

#include <tlx/container/btree_map.hpp>
#include "uring/iface.h"



static constexpr uint32_t kDefaultTXN = 10000;
static constexpr uint16_t kDefaultSQBatch = 8;

static std::random_device dev;
static std::mt19937 rng(dev());
static std::uniform_int_distribution<std::mt19937::result_type> dist(INT64_MIN,
                                                                     INT64_MAX);
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

static void handle_request(uring::server_iface *sock,
                           kv_packet<kv_request> *req, int idx) {
  auto key = req->payload.key;
  auto it = store.find(key);
  auto *completion =
      static_cast<kv_packet<kv_completion> *>(sock->pool.alloc());
  completion->id = req->id;
  completion->pt = req->pt;
  completion->payload.key = req->payload.key;
  if (it == store.end()) {
    completion->payload.reponse = response_t::FAILURE;
    completion->payload.val = 0;
  } else {
    completion->payload.reponse = response_t::SUCCESS;
    completion->payload.val = it->second;
  }
  auto tx_idx = sock->ctx->next_free_tx_buffer(completion);
  sock->prepare_send(completion, sizeof(*completion), tx_idx,
                     sock->clients[idx]);
}

static uint64_t request_batch(uring::client_iface *st, uint64_t t, uint8_t bs) {
  for (auto i = 0u; i < bs; ++i) {
    uint8_t *buf = static_cast<uint8_t *>(st->pool.alloc());
    if (!buf)
      break;
    create_kv_request(buf, t++, dist(rng));
    st->ctx->next_free_tx_buffer(buf);
    st->prepare_send(buf, sizeof(kv_packet<kv_request>), 0,
                     st->fd);
  }
  return t;
}

static uint64_t process_completions(uring::client_iface *st) {
  unsigned head = 0;  
  struct io_uring_cqe *cqe;
  io_uring_for_each_cqe(&st->ctx->ring, head, cqe){
    st->handle_cqe(cqe,
                      [&](void *buf, size_t size, unsigned sidx) {
                        (void)size, (void)sidx, (void)buf;
                      });
  }
  io_uring_cq_advance(&st->ctx->ring, head);
  return head;
}

static void parse(uring::server_iface *st, uint8_t *data, size_t size, unsigned idx) {
  unsigned i = 0;
  for (; i < size;) {
    auto *req = reinterpret_cast<kv_packet<kv_request> *>(data + i);
    if (size - i < sizeof(*req))
      break;
    i += sizeof(*req);
    handle_request(st, req, idx);
  }
  std::memmove(data, data + i, size - i);
}

static int client_fun(struct sockaddr_in* addr){
    uring::client_iface iface{};
    struct io_uring_cqe *cqe;
    iface.ctx->setup();
    iface.setup(0);
    uint64_t t = 0, c = 0;
    iface.uring_connect(addr);
    iface.uring_submit_and_wait();
    io_uring_peek_cqe(&iface.ctx->ring, &cqe);
    io_uring_cq_advance(&iface.ctx->ring, 1);
    if(cqe->res < 0){
        fprintf(stderr, "Failed to connect: %d\n", cqe->res);
        return cqe->res;
    }
    while (kDefaultTXN > t) {
         c += process_completions(&iface);
         t = request_batch(&iface, t, kDefaultSQBatch);
    }

    while (c < kDefaultTXN)
        c += process_completions(&iface);
    return 0;
}


static int server_fun(int port_arg){
    int ret;
    prepare();
    struct io_uring_cqe *cqe;
    uring::server_iface iface{};
    iface.ctx->setup();
    iface.setup(port_arg);
    unsigned head = 0;
    ret = iface.uring_prepare_listen();
    if(ret){
        fprintf(stderr, "Set listen failed: %s\n", strerror(-ret));
        return ret;
    }

    ret = iface.uring_prepare_accept();
    if(ret){
        fprintf(stderr, "Prepare accept failed: %s\n", strerror(-ret));
        return ret;
    }

    while (true) {
      ret = iface.uring_submit_and_wait(&cqe);
      if(ret == -ETIME)
          continue;
      head = 0;
      io_uring_for_each_cqe(&iface.ctx->ring, head, cqe) {
        ret = iface.handle_cqe(
            cqe, [&](void *buf, size_t size, unsigned sidx) {
              auto &slt = iface.connection_state(sidx);
              std::memcpy(slt.reassemble_buffer.data() + slt.off, buf, size);
              slt.off += size;
              parse(&iface, slt.reassemble_buffer.data(), slt.off, sidx);
            }); 
        if(ret)
            break;
      }
      io_uring_cq_advance(&iface.ctx->ring, head);
    }
    return 0;
}

int main(int argc, char *argv[]) {
  uint16_t port_arg = 0;
  int opt, ret = 0;
  bool is_client = false;
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
  if(is_client)
      ret = client_fun(&addr);
  else
      ret = server_fun(port_arg);
  return ret;
}
