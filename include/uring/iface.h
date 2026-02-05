#pragma once

#include <array>
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
#include <sys/mman.h>
#include <vector>

namespace uring {
static constexpr int kQueueDepth = 256;
static constexpr int kNumBuffer = kQueueDepth * 16;
static constexpr int kBufShift = 12;
static constexpr unsigned kCQEntries = kQueueDepth * 8;
static constexpr int kMaxClientFd = 128;
static constexpr int kDefaultBufferSize = 256;

static constexpr uint64_t tag_send(unsigned idx) {
  return static_cast<uint64_t>(idx) * 2;
}

static constexpr uint64_t tag_recv(unsigned idx) {
  return static_cast<uint64_t>(idx) * 2 + 1;
}

static constexpr unsigned untag(uint64_t user_data) { return user_data >> 1; }

template <typename T>
int process_cqe_recv(T *st, struct io_uring_cqe *cqe, int fd, unsigned sidx,
                     auto &&f);

template <typename T> int add_recv(T *iface, int fd, int idx);

struct slot {
  static constexpr unsigned kDefaultAssemblyBufferSize = 256 * 1024;
  struct msg_buf {
    char *buf;
    size_t ptr, len;
  };
  uint16_t idx = 0;
  std::vector<uint8_t> reassemble_buffer;
  unsigned off = 0;

  slot() : reassemble_buffer(kDefaultAssemblyBufferSize) {}
};

template <size_t elemsize>
  requires(elemsize >= 64)
struct buffer_pool {
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

struct uring_context {
  struct io_uring ring;
  struct io_uring_buf_ring *buf_ring;
  unsigned char *buffer_base;
  int buf_shift = kBufShift;
  size_t buf_ring_size;
  std::array<void *, kQueueDepth> tx_buffer;
  uint16_t next_to_use = 0;

  uint16_t next_free_tx_buffer(void *buf) {
    tx_buffer[next_to_use] = buf;
    auto idx = next_to_use;
    next_to_use = (next_to_use + 1) & (kQueueDepth - 1);
    return idx;
  }

  bool get_sqe(struct io_uring_sqe **sqe) {
    *sqe = io_uring_get_sqe(&ring);

    if (!*sqe) {
      io_uring_submit(&ring);
      *sqe = io_uring_get_sqe(&ring);
    }
    if (!*sqe) {
      fprintf(stderr, "cannot get sqe\n");
      return true;
    }
    return false;
  }

  size_t buffer_size() const { return 1U << buf_shift; }

  unsigned char *get_buffer(int idx) {
    return buffer_base + (idx << buf_shift);
  }

  int setup() {
    struct io_uring_params params{};
    int ret;

    params.cq_entries = kCQEntries;
    params.flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN |
                   IORING_SETUP_CQSIZE;
    ret = io_uring_queue_init_params(kQueueDepth, &ring, &params);
    if (ret) {
      fprintf(stderr, "Queue init failed: %s\n", strerror(-ret));
      return ret;
    }

    ret = setup_buffer_pool();
    if (ret)
      io_uring_queue_exit(&ring);
    return 0;
  }

  int setup_buffer_pool() {
    int ret, i;
    void *mapped;
    struct io_uring_buf_reg reg = {.ring_addr = 0,
                                   .ring_entries = kNumBuffer,
                                   .bgid = 0,
                                   .flags = 0,
                                   .resv = {0}};

    buf_ring_size = (sizeof(struct io_uring_buf) + buffer_size()) * kNumBuffer;
    mapped = mmap(NULL, buf_ring_size, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
    if (mapped == MAP_FAILED) {
      fprintf(stderr, "buf_ring mmap: %s\n", strerror(errno));
      return -1;
    }
    buf_ring = (struct io_uring_buf_ring *)mapped;

    io_uring_buf_ring_init(buf_ring);

    reg = (struct io_uring_buf_reg){.ring_addr = (unsigned long)buf_ring,
                                    .ring_entries = kNumBuffer,
                                    .bgid = 0,
                                    .flags = 0,
                                    .resv = {0}};
    buffer_base =
        (unsigned char *)buf_ring + sizeof(struct io_uring_buf) * kNumBuffer;

    ret = io_uring_register_buf_ring(&ring, &reg, 0);
    if (ret) {
      fprintf(stderr, "buf_ring init failed: %s\n", strerror(-ret));
      return ret;
    }

    for (i = 0; i < kNumBuffer; i++) {
      io_uring_buf_ring_add(buf_ring, get_buffer(i), buffer_size(), i,
                            io_uring_buf_ring_mask(kNumBuffer), i);
    }
    io_uring_buf_ring_advance(buf_ring, kNumBuffer);

    return 0;
  }

  struct io_uring_sqe *get_slot() { return io_uring_get_sqe(&ring); }

  ~uring_context() {
    munmap(buf_ring, buf_ring_size);
    io_uring_queue_exit(&ring);
  }
};

struct iface_base {

  uint16_t port;

  std::unique_ptr<uring_context> ctx;

  buffer_pool<kDefaultBufferSize> pool;

  iface_base() : ctx(std::make_unique<uring_context>()), pool(512) {}

  void prepare_send(void *buf, size_t len, unsigned idx, int peer_fd,
                    struct io_uring_sqe *sqe) {
    assert(sqe && "No free sqe");
    io_uring_prep_send(sqe, peer_fd, buf, len, 0);
    io_uring_sqe_set_data64(sqe, tag_send(idx));
  }

  void prepare_send_zc(void *buf, size_t len, unsigned idx, int peer_fd,
                       struct io_uring_sqe *sqe) {
    assert(sqe && "No free sqe");
    io_uring_prep_send_zc(sqe, peer_fd, buf, len, 0, 0);
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
    auto tx_idx = untag(cqe->user_data);
    auto *buf = ctx->tx_buffer[tx_idx];
    pool.free(buf);
    return 0;
  }
};

struct client_iface : iface_base {
  int fd;
  std::vector<uint8_t> reassembly;
  size_t off = 0;

  client_iface() : iface_base(), reassembly(slot::kDefaultAssemblyBufferSize) {}

  int uring_connect(struct sockaddr_in *addr) {
    auto *seq = io_uring_get_sqe(&ctx->ring);
    io_uring_prep_connect(seq, fd,
                          reinterpret_cast<const struct sockaddr *>(addr),
                          sizeof(*addr));
    return 0;
  }

  int setup(int port_arg) { 
      int ret = setup_base(port_arg, fd); 
      if(ret < 0){
          fprintf(stderr, "Setting up socket failed %s\n", strerror(-ret));
          return ret;
      }
      return 0;
  }

  int prepare_recv(){
      return add_recv(this, fd, tag_recv(0));
  }

  int handle_cqe(struct io_uring_cqe *cqe, auto &&f) {
    switch (cqe->user_data & 1) {
    case 0: {
      process_cqe_send(cqe);
      break;
    }
    case 1: {          
      return process_cqe_recv(this, cqe, fd, tag_recv(0), f);
      break;
    }
    }
    return 0;
  }
};

struct server_iface : iface_base {
  std::vector<int> clients;
  std::vector<slot> con_state;
  std::deque<int> free_slots;

  server_iface()
      : iface_base(), clients(kMaxClientFd + 1), con_state(kMaxClientFd + 1) {

    for (auto idx = 1; idx < kMaxClientFd + 1; ++idx)
      free_slots.push_back(idx);
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

  int handle_accept(struct io_uring_cqe *cqe) {
    if (cqe->res > 0) {
      auto idx = free_slots.front();
      free_slots.pop_front();
      clients[idx] = cqe->res;
      con_state[idx] = {};
      add_recv(this, cqe->res, idx);
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
      if(idx == 0)
          return handle_accept(cqe);
      else
        return process_cqe_recv(this, cqe, clients[idx], idx, f);
    }
    }
    return 0;
  }
};

template <typename T> int add_recv(T *iface, int fd, int idx) {
  struct io_uring_sqe *sqe;

  if (iface->ctx->get_sqe(&sqe)) {
    fprintf(stderr, "No free sqe\n");
    return -1;
  }

  io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);

  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = 0;
  io_uring_sqe_set_data64(sqe, tag_recv(idx));
  return 0;
}

__inline void recycle_buffer(uring_context *ctx, int idx) {
  io_uring_buf_ring_add(ctx->buf_ring, ctx->get_buffer(idx), ctx->buffer_size(),
                        idx, io_uring_buf_ring_mask(kNumBuffer), 0);
  io_uring_buf_ring_advance(ctx->buf_ring, 1);
}

__inline int process_cqe_send(iface_base *st, struct io_uring_cqe *cqe) {
  if (cqe->res < 0)
    fprintf(stderr, "bad send %s\n", strerror(-cqe->res));
  auto tx_idx = untag(cqe->user_data);
  auto *buf = st->ctx->tx_buffer[tx_idx];
  st->pool.free(buf);
  return 0;
}

template <typename T>
int process_cqe_recv(T *st, struct io_uring_cqe *cqe, int fd, unsigned sidx,
                     auto &&f) {
  assert(fd > 0);  
  int ret, idx;
  if (!(cqe->flags & IORING_CQE_F_MORE)) {
    ret = add_recv<T>(st, fd, sidx);
    if (ret)
      return ret;
  }
  if (cqe->res == -ENOBUFS)
    return 0;

  if (!(cqe->flags & IORING_CQE_F_BUFFER) || cqe->res < 0) {
    fprintf(stderr, "recv cqe bad res %s\n", strerror(-cqe->res));
    if (cqe->res == -EFAULT || cqe->res == -EINVAL)
      fprintf(stderr, "NB: This requires a kernel version >= 6.0\n");
    return -1;
  }
  idx = cqe->flags >> 16; // 16 bits is bid
  auto *buf = st->ctx->get_buffer(idx);
  ret = f(buf, cqe->res, sidx);
  if(ret)
      return -1;
  recycle_buffer(st->ctx.get(), idx);
  return 0;
}
} // namespace uring
