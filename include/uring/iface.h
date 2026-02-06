#pragma once

#include "uring/qpair.h"
#include "util.h"
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <err.h>
#include <errno.h>
#include <liburing.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <vector>

namespace uring {
static constexpr int kMaxClientFd = 128;
static constexpr int kDefaultBufferSize = 1 << kBufShift;

static constexpr uint64_t tag_send(unsigned idx) {
  return static_cast<uint64_t>(idx) * 2;
}

static constexpr uint64_t tag_recv(unsigned idx) {
  return static_cast<uint64_t>(idx) * 2 + 1;
}

static int disable_nagle(int fd) {
  int flag = 1;
  return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));
}

static constexpr unsigned untag(uint64_t user_data) { return user_data >> 1; }

template <typename T>
int process_cqe_recv(T *st, struct io_uring_cqe *cqe, int fd, unsigned sidx,
                     auto &&f);

template <typename T> int add_recv(T *iface, int fd, int idx);

using rx_pair = std::pair<unsigned, size_t>;

struct reassembly_buffer {
  std::vector<uint8_t> buffer;
  unsigned off = 0;
  reassembly_buffer(size_t size) : buffer(size) {}
 
  bool enough_space(size_t len) const { return buffer.size() - off >= len; }

  uint8_t *data(){ return buffer.data(); }

  void reset(unsigned noff){ off = noff; }

  void cpy(void *buf, size_t size) {
    std::memcpy(buffer.data() + off, buf, size);
    off += size;
  }
};

struct slot {
  static constexpr unsigned kDefaultAssemblyBufferSize = 256 * 1024;
  uint16_t idx = 0;
  reassembly_buffer rbuffer;
  list_hook link;
  std::deque<rx_pair> incoming;
  bool rx_pending = false;
  slot() : rbuffer(kDefaultAssemblyBufferSize) {}
};

template <size_t elemsize>
  requires(elemsize >= 64)
struct buffer_pool {
  static constexpr size_t kElemSize = elemsize;
  struct [[gnu::packed]] header {
    header *next;
  };
  uint8_t *base;
  size_t size;
  header *next = nullptr;
  buffer_pool(size_t n) {
    auto psize = sysconf(_SC_PAGE_SIZE);
    size = (elemsize * n + psize - 1) & ~(psize - 1);
    base = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANON, -1, 0));
    for (auto i = 0u; i < n; ++i) {
      auto *next_ptr =
          (i + 1 < n) ? reinterpret_cast<header *>(base + (i + 1) * elemsize)
                      : nullptr;
      new (base + i * elemsize) header{next_ptr};
    }
    next = reinterpret_cast<header *>(base);
  }

  void *alloc() {
    if (next == nullptr)
      return nullptr;
    auto *area = next;
    next = area->next;
    return area;
  }

  void free(void *data) { next = new (data) header{next}; }
};

struct iface_base {

  uint16_t port;
  std::unique_ptr<qpair> ctx;

  buffer_pool<kDefaultBufferSize> pool;

  iface_base() : ctx(qpair::create()), pool(kNumBuffer) {
    assert(ctx.get() && "No valid uring ctx created");
  }

  void prepare_send(void *buf, size_t len, uint64_t idx, int peer_fd,
                    struct io_uring_sqe *sqe) {
    assert(sqe && "No free sqe");
    io_uring_prep_send(sqe, peer_fd, buf, len, MSG_WAITALL);
    assert((idx & 1) == 0);
    io_uring_sqe_set_data64(sqe, idx);
  }

  void prepare_send_zc(void *buf, size_t len, uint64_t idx, int peer_fd,
                       struct io_uring_sqe *sqe) {
    assert(sqe && "No free sqe");
    io_uring_prep_send_zc(sqe, peer_fd, buf, len, MSG_WAITALL, 0);
    assert((idx & 1) == 0);
    io_uring_sqe_set_data64(sqe, tag_send(idx));
  }

  int uring_submit_and_wait(struct io_uring_cqe **cqe) {
    static constexpr unsigned kDefaultTimeout = 10000;
    struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = kDefaultTimeout};
    return io_uring_submit_and_wait_timeout(&ctx->ring, cqe, 1, &ts, nullptr);
  }

  int uring_submit_and_wait() {
    return io_uring_submit_and_wait(&ctx->ring, 1);
  }

  int uring_submit() { return io_uring_submit(&ctx->ring); }

  int setup_base(int port_arg, int &fd) {
    port = port_arg <= 0 ? 0 : htons(port_arg);
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      fprintf(stderr, "sock init failed %s\n", strerror(errno));
      return fd;
    }

    return 0;
  }

  int process_cqe_send(struct io_uring_cqe *cqe) {
    if (cqe->res < 0)
      fprintf(stderr, "bad send %s\n", strerror(-cqe->res));
    if (cqe->flags & IORING_CQE_F_MORE)
      return 0;
    auto *buf = std::bit_cast<void *>(cqe->user_data);
    assert(cqe->flags == 0);
    pool.free(buf);
    return 0;
  }
};

struct client_iface : iface_base {
  int fd;
  reassembly_buffer rbuffer;

  client_iface() : iface_base(), rbuffer(slot::kDefaultAssemblyBufferSize) {}

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
    disable_nagle(fd);
    return 0;
  }

  int prepare_recv() { return add_recv(this, fd, 0); }

  int handle_cqe(struct io_uring_cqe *cqe, auto &&f) {
    switch (cqe->user_data & 1) {
    case 0:
      return process_cqe_send(cqe);
    case 1:
      return process_cqe_recv(this, cqe, fd, 0, f);
    }
    return 0;
  }
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

  slot &connection_state(unsigned idx) { return con_state[idx]; }

  int setup(int port_arg) { return setup_base(port_arg, clients.front()); }

  int uring_prepare_listen() {
    int enable = 1;
    int ret = setsockopt(clients.front(), SOL_SOCKET, SO_REUSEADDR, &enable,
                         sizeof(enable));
    if (ret) {
      fprintf(stderr, "Failed to set socket op: %s\n", strerror(errno));
      return ret;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = port,
                               .sin_addr = {INADDR_ANY},
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

  int handle_close(int idx) {
    auto *sqe = ctx->get_sqe();
    assert(sqe);
    io_uring_prep_cancel64(sqe, static_cast<uint64_t>(clients[idx]),
                           IORING_ASYNC_CANCEL_FD);
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
    free_slots.push_front(idx);
    close(clients[idx]);
    con_state[idx].link.unlink();
    return 0;
  }

  int handle_accept(struct io_uring_cqe *cqe) {
    if (cqe->res > 0) {
      auto idx = free_slots.front();
      free_slots.pop_front();
      clients[idx] = cqe->res;
      printf("%u\n", idx);
      con_state[idx] = {};
      add_recv(this, cqe->res, idx);
      disable_nagle(cqe->res);
      active.push_front(con_state[idx]);
      return 0;
    }
    return cqe->res;
  }

  int handle_cqe(struct io_uring_cqe *cqe, auto &&f) {
    switch (cqe->user_data & 1) {
    case 0: {
      return process_cqe_send(cqe);
    }
    case 1: {
      auto idx = untag(cqe->user_data);
      if (idx == 0)
        return handle_accept(cqe);
      else {
        if (cqe->res == -ECONNRESET)
          return handle_close(idx);
        else
          return process_cqe_recv(this, cqe, clients[idx], idx, f);
      }
    }
    }
    return 0;
  }
};

template <typename T> int add_recv(T *iface, int fd, int idx) {
  struct io_uring_sqe *sqe;
  sqe = iface->ctx->get_sqe();
  if (!sqe) {
    assert(0);  
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

template <typename T>
int process_cqe_recv(T *st, struct io_uring_cqe *cqe, int fd, unsigned sidx,
                     auto &&f) {
  assert(fd > 0);
  int ret, idx;
  if (!(cqe->flags & IORING_CQE_F_MORE)) {
    ret = add_recv<T>(st, fd, sidx);
    assert(!ret);
  }
  if (cqe->res == -ENOBUFS)
    return 0;

  if (!(cqe->flags & IORING_CQE_F_BUFFER) || cqe->res < 0) {
    if (cqe->res == -EFAULT || cqe->res == -EINVAL)
      fprintf(stderr, "NB: This requires a kernel version >= 6.0\n");
    return -1;
  }
  idx = cqe->flags >> 16; // 16 bits is bid
  auto *buf = st->ctx->get_buffer(idx);
  ret = f(buf, cqe->res, sidx);
  recycle_buffer(st->ctx.get(), idx);
  return ret;
}
} // namespace uring
