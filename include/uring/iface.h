#pragma once

#include "uring/qpair.h"
#include "uring/tcp.h"

#include "util.h"
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <err.h>
#include <errno.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace uring {
static constexpr int kMaxClientFd = 128;
static constexpr int kMaxClientFdTag = 2 * kMaxClientFd + 1;
static constexpr int kSetSockTag = kMaxClientFd + 1;
static constexpr int kGetSockTCPInfoTag = kSetSockTag + 2;
static constexpr int kCancelTag = kSetSockTag + 4;
static constexpr int kDefaultBufferSize = 32;

static constexpr uint64_t tag_send(unsigned idx) {
  return static_cast<uint64_t>(idx) * 2;
}

static constexpr uint64_t tag_recv(unsigned idx) {
  return static_cast<uint64_t>(idx) * 2 + 1;
}

static constexpr unsigned untag(uint64_t user_data) { return user_data >> 1; }

struct slot;
template <typename T> int add_recv(T *iface, int fd, int idx);
template <typename T>
int process_cqe_recv(T *st, struct io_uring_cqe *cqe, int fd, unsigned sidx,
                     auto &&f);

__inline void recycle_buffer(qpair *ctx, int idx);

using rx_pair = std::pair<unsigned, size_t>;

struct reassembly_buffer {
  std::vector<uint8_t> buffer;
  unsigned off = 0;
  reassembly_buffer(size_t size) : buffer(size) {}

  bool enough_space(size_t len) const { return buffer.size() - off >= len; }

  uint8_t *data() { return buffer.data(); }

  void reset(unsigned noff) { off = noff; }

  void cpy(void *buf, size_t size) {
    assert(off + size <= buffer.size());
    std::memcpy(buffer.data() + off, buf, size);
    off += size;
  }
};

struct data_buffer {
  std::vector<uint8_t> buffer;
  uint32_t head = 0, tail = 0;

  data_buffer(size_t len) : buffer(len) {}

  uint8_t *reserve(size_t amount) {
    if (buffer.size() - tail < amount)
      return nullptr;
    auto *data = buffer.data() + tail;
    tail += amount;
    return data;
  }

  void finalize() {
    std::memmove(buffer.data(), buffer.data() + head, tail - head);
    tail -= head;
    head = 0;
  }

  void mark_tx(size_t sz) {
    head += sz;
    assert(tail >= head);
    finalize();
  }

  uint8_t *front() { return buffer.data() + head; }

  size_t size() { return tail - head; }
};

struct slot {
  static constexpr unsigned kDefaultAssemblyBufferSize = 128 * 1024;
  static constexpr unsigned kDefaultTxBufferSize = 256 * 1024;
  uint16_t idx = 0;
  bool recv_scheduled = false;
  bool tx_inflight = false;
  bool eof = false;
  std::deque<rx_pair> incoming;
  data_buffer tx_buffer;
  reassembly_buffer rbuffer;
  list_hook link;
  slot()
      : tx_buffer(kDefaultTxBufferSize), rbuffer(kDefaultAssemblyBufferSize) {}
  void reset() {
    recv_scheduled = false;
    tx_inflight = false;
    eof = false;
    tx_buffer.head = tx_buffer.tail = 0;
    rbuffer.off = 0;
    assert(incoming.empty());
    assert(!link.is_linked());
  }

  void prepare_send(void *buf, size_t len, unsigned idx, int fd,
                    struct io_uring_sqe *sqe) {
    assert(!tx_inflight);
    tx_inflight = true;
    io_uring_prep_send(sqe, fd, buf, len, 0);
    sqe->ioprio |= IORING_RECVSEND_POLL_FIRST;
    io_uring_sqe_set_data64(sqe, tag_send(idx));
  }
};

struct iface_base {
  int flag;
  uint16_t port;

  std::deque<std::pair<int, int>> rx_renew;
  struct tcp_info info;
  struct io_uring_napi napi{};
  std::unique_ptr<qpair> ctx;
  struct io_uring_sqe *seq = nullptr;

  iface_base() : ctx(qpair::create()) {
    assert(ctx.get() && "No valid uring ctx created");
  }

  void drain_incoming(slot &slt) {
    while (!slt.incoming.empty()) {
      auto [idx, size] = slt.incoming.front();
      if (!slt.rbuffer.enough_space(size))
        break;
      auto *buf = ctx->get_buffer(idx);
      slt.rbuffer.cpy(buf, size);
      uring::recycle_buffer(ctx.get(), idx);
      slt.incoming.pop_front();
    }
  }

  void release_incoming(slot& slt){
      while(!slt.incoming.empty()){
          auto [idx, _] = slt.incoming.front();
          recycle_buffer(ctx.get(), idx);
          slt.incoming.pop_front();
      }
  }

  int uring_submit_and_wait() {
    return io_uring_submit_and_wait(&ctx->ring, 1);
  }

  int uring_submit_and_get_events() {
    return io_uring_submit_and_get_events(&ctx->ring);
  }

  int uring_submit() { return io_uring_submit(&ctx->ring); }

  int setup_base(int port_arg, int &fd) {
    port = port_arg <= 0 ? 0 : htons(port_arg);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      fprintf(stderr, "sock init failed %s\n", strerror(errno));
      return fd;
    }
    napi.prefer_busy_poll = true;
    napi.busy_poll_to = 10;
    io_uring_register_napi(&ctx->ring, &napi);
    return 0;
  }

  void uring_socketopt(int fd, int optname, void *optval, socklen_t len) {
    auto *sqe = io_uring_get_sqe(&ctx->ring);
    io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_SETSOCKOPT, fd, IPPROTO_TCP,
                           optname, optval, len);
    io_uring_sqe_set_data64(sqe, kSetSockTag);
  }

  void uring_set_no_delay(int fd) {
    flag = 1;
    uring_socketopt(fd, TCP_NODELAY, &flag, sizeof(flag));
  }

  void uring_set_bbr(int fd) {
    uring_socketopt(fd, TCP_CONGESTION,
                    std::bit_cast<void *>(&tcp::bbr_congestion),
                    sizeof(tcp::bbr_congestion));
  }

  void uring_gettcpstats(int fd) {
    auto *sqe = ctx->get_sqe();
    io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_GETSOCKOPT, fd, IPPROTO_TCP,
                           TCP_INFO, &info, sizeof(tcp_info));
    io_uring_sqe_set_data64(sqe, kGetSockTCPInfoTag);
  }

  virtual ~iface_base() = default;
};

struct client_iface : iface_base {
  int fd;
  slot slt;
  client_iface() : iface_base(), slt() {}

  int uring_connect(struct sockaddr_in *addr) {
    auto *seq = io_uring_get_sqe(&ctx->ring);
    io_uring_prep_connect(seq, fd,
                          reinterpret_cast<const struct sockaddr *>(addr),
                          sizeof(*addr));
    return 0;
  }

  int setup(int port_arg) {
    int ret = setup_base(port_arg, fd);
    if (ret < 0) {
      fprintf(stderr, "Setting up socket failed %s\n", strerror(-ret));
      return ret;
    }
    tcp::disable_nagle(fd);
    tcp::change_congestion_control(fd, tcp::bbr_congestion);
    return 0;
  }

  slot &slot_at(int) { return slt; }

  int prepare_recv() { return add_recv(this, fd, 0); }

  int process_cqe_send(struct io_uring_cqe *cqe) {
    if (cqe->res < 0) {
      fprintf(stderr, "bad send %s\n", strerror(-cqe->res));
      assert(0);
      return cqe->res;
    }
    auto idx = untag(cqe->user_data);
    assert(idx == 0);
    assert(slt.tx_inflight);
    slt.tx_inflight = false;
    slt.tx_buffer.mark_tx(cqe->res);
    assert(cqe->flags == 0);
    return 0;
  }

  int process_cmd_cqe(io_uring_cqe *cqe) {
    switch (cqe->user_data) {
    case kSetSockTag:
    case kGetSockTCPInfoTag:
      return 0;
    case kCancelTag:
      return 0;
    default:
      assert(0);
    }
    return 0;
  }

  int handle_cqe(struct io_uring_cqe *cqe, auto &&f) {
    switch (cqe->user_data & 1) {
    case 0:
      return process_cqe_send(cqe);
    case 1:
      if (cqe->user_data > kMaxClientFdTag)
        return process_cmd_cqe(cqe);
      return process_cqe_recv(this, cqe, fd, 0, f);
    }
    return 0;
  }

  void handle_cancel(unsigned idx){
      (void)idx;
  }
  int handle_close(int idx) {
    (void)idx;
    auto *sqe = ctx->get_sqe();
    assert(sqe);
    io_uring_prep_cancel64(sqe, static_cast<uint64_t>(fd),
                           IORING_ASYNC_CANCEL_FD);
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
    close(fd);
    return 0;
  }

  ~client_iface() override { close(fd); }
};

struct server_iface : iface_base {
  std::vector<int> clients;
  std::vector<slot> con_state;
  std::deque<int> free_slots;
  intrusive_list_t<slot> active;

  server_iface()
      : iface_base(), clients(kMaxClientFd + 1), con_state(kMaxClientFd + 1) {
    for (auto idx = 1; idx < kMaxClientFd + 1; ++idx)
      free_slots.push_back(idx);
    unsigned idx = 0;
    for (auto &cs : con_state) {
      cs.idx = idx++;
    }
  }

  int process_cqe_send(struct io_uring_cqe *cqe) {
    if (cqe->res < 0) {
      fprintf(stderr, "bad send %s\n", strerror(-cqe->res));
      return cqe->res;
    }
    auto idx = untag(cqe->user_data);
    auto &slt = con_state[idx];
    assert(slt.tx_inflight || cqe->res == 0);
    slt.tx_inflight = false;
    slt.tx_buffer.mark_tx(cqe->res);
    assert(cqe->flags == 0);
    return 0;
  }

  slot &slot_at(int idx) { return con_state[idx]; }

  int setup(int port_arg) { return setup_base(port_arg, clients.front()); }

  int prepare_listen(in_addr_t s_addr) {
    int enable = 1;
    int ret = setsockopt(clients.front(), SOL_SOCKET, SO_REUSEADDR, &enable,
                         sizeof(enable));
    if (ret) {
      fprintf(stderr, "Failed to set socket op: %s\n", strerror(errno));
      return ret;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = port,
                               .sin_addr = {s_addr},
                               .sin_zero = {0}};
    ret = bind(clients.front(), reinterpret_cast<struct sockaddr *>(&addr),
               sizeof(addr));
    if (ret) {
      fprintf(stderr, "Binding socket failed: %s\n", strerror(-ret));
      return ret;
    }
    return listen(clients.front(), 1 << 10);
  }

  int uring_prepare_accept() {
    auto sqe = io_uring_get_sqe(&ctx->ring);
    io_uring_prep_multishot_accept(sqe, clients.front(), nullptr, nullptr, 0);
    io_uring_sqe_set_data64(sqe, tag_recv(0));
    return 0;
  }

  int handle_cancel(int idx) {
    auto& slt = con_state[idx];
    if(slt.eof)
        return 0;
    slt.eof = true;
    auto *sqe = ctx->get_sqe();
    assert(sqe);
    io_uring_prep_cancel_fd(sqe, clients[idx], IORING_ASYNC_CANCEL_ALL);
    io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(idx) << 32 | kCancelTag);
    return 0;
  }

  int handle_close(unsigned idx){
      close(clients[idx]);
      auto &slt = con_state[idx];
      release_incoming(slt);
      free_slots.push_front(slt.idx);
      slt.link.unlink();
      return 0;
  }

  int handle_accept(struct io_uring_cqe *cqe) {
    if (cqe->res > 0) {
      auto idx = free_slots.front();
      free_slots.pop_front();
      clients[idx] = cqe->res;
      auto &slt = con_state[idx];
      slt.reset();
      add_recv(this, cqe->res, idx);
      io_uring_register_napi(&ctx->ring, &napi);
      tcp::disable_nagle(cqe->res);
      tcp::change_congestion_control(cqe->res, tcp::bbr_congestion);
      active.push_front(con_state[idx]);
      assert(con_state[idx].idx == idx);
      return 0;
    }
    return cqe->res;
  }

  int process_cmd_cqe(io_uring_cqe *cqe) {
    switch (cqe->user_data & ((1ull << 32) - 1)) {
    case kSetSockTag:
    case kGetSockTCPInfoTag:
      return 0;
    case kCancelTag:{
      auto idx = cqe->user_data >> 32;                  
      printf("canceling %llu\n", idx);
      handle_close(idx);
      return 0;
                    }
    default:
      assert(0);
    }
    return 0;
  }

  int handle_cqe(struct io_uring_cqe *cqe, auto &&f) {
    switch (cqe->user_data & 1) {
    case 0: {
      return process_cqe_send(cqe);
    }
    case 1: {
      if (cqe->user_data > kMaxClientFdTag)
        return process_cmd_cqe(cqe);
      auto idx = untag(cqe->user_data);
      if (idx == 0)
        return handle_accept(cqe);
      else {
        if (cqe->res == -ECONNRESET)
          return handle_cancel(idx);
        else
          return process_cqe_recv(this, cqe, clients[idx], idx, f);
      }
    }
    }
    return 0;
  }

  ~server_iface() override { close(clients.front()); }
};

template <typename T> int add_recv(T *iface, int fd, int idx) {
  struct io_uring_sqe *sqe;
  sqe = iface->ctx->get_sqe();
  if (!sqe) {
    iface->rx_renew.emplace_back(fd, idx);
    return -1;
  }

  io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);

  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = 0;
  io_uring_sqe_set_data64(sqe, tag_recv(idx));
  return 0;
}

__inline void recycle_buffer(qpair *ctx, int idx) {
  io_uring_buf_ring_add(ctx->buf_ring, ctx->get_buffer(idx), ctx->buffer_size(),
                        idx, io_uring_buf_ring_mask(kNumBuffer), 0);
  io_uring_buf_ring_advance(ctx->buf_ring, 1);
}

template <typename T> void drain_rx_renew(T *iface) {
  while (!iface->rx_renew.empty()) {
    auto [fd, idx] = iface->rx_renew.front();
    if (add_recv(iface, fd, idx))
      break;
    iface->rx_renew.pop_front();
  }
}

template <typename T>
int process_cqe_recv(T *st, struct io_uring_cqe *cqe, int fd, unsigned sidx,
                     auto &&f) {
  assert(fd > 0);
  int ret, idx;
  if (!(cqe->flags & IORING_CQE_F_MORE))
    ret = add_recv<T>(st, fd, sidx);

  if (cqe->res == -ENOBUFS)
    return 0;

  if (!(cqe->flags & IORING_CQE_F_BUFFER) || cqe->res < 0) {
    ret = cqe->res;
    if (cqe->res == 0){
      st->handle_cancel(sidx);
    }
    return ret;
  }

  idx = cqe->flags >> 16;
  ret = f(idx, cqe->res, sidx);
  return ret;
}
} // namespace uring
