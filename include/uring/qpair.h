#pragma once

#include <cassert>
#include <cstdio>
#include <cstring>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <sys/mman.h>
#include <memory>
#include <span>
#include <thread>

namespace uring {
static constexpr int kQueueDepth = 512;
static constexpr int kNumBuffer = kQueueDepth * 4;
static constexpr int kBufSize = 2048 + 64;
static constexpr unsigned kCQEntries = kQueueDepth * 8;

struct qpair {
    std::thread::id id;  
  io_uring ring{};
  io_uring_buf_ring *buf_ring = nullptr;
  unsigned char *buffer_base = nullptr;
  size_t buf_ring_size = 0;

  static std::unique_ptr<qpair> create() {  
    auto qp = std::make_unique<qpair>();  
    if (auto ret = qp->setup_ring(); ret != 0)
      return nullptr;
    if (auto ret = qp->setup_buffer_pool(); ret != 0) {
      io_uring_queue_exit(&qp->ring);
      return nullptr;
    }
    qp->id = std::this_thread::get_id();
    return qp;
  }

  size_t buffer_size() const { return kBufSize; }

  unsigned char *get_buffer(int idx) {
    return buffer_base + (idx * kBufSize);
  }

  io_uring_sqe *get_sqe() {
    auto *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      io_uring_submit(&ring);
      sqe = io_uring_get_sqe(&ring);
    }
    return sqe;
  }

  int peek_batch_cqe(std::span<struct io_uring_cqe*> cqes){
      return io_uring_peek_batch_cqe(&ring, cqes.data(), cqes.size());
  }

  ~qpair() {
    if (buf_ring)
      munmap(buf_ring, buf_ring_size);
    if (ring.ring_fd > 0)
      io_uring_queue_exit(&ring);
  }

  qpair() = default;

private:

  int setup_ring() {
    io_uring_params params{};
    params.cq_entries = kCQEntries;
    params.sq_entries = kQueueDepth;
    params.flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN |
                   IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER;
    int ret = io_uring_queue_init_params(kQueueDepth, &ring, &params);
    if (ret) {
      fprintf(stderr, "Queue init failed: %s\n", strerror(-ret));
      return ret;
    }
    return 0;
  }

  int setup_buffer_pool() {
    buf_ring_size = (sizeof(io_uring_buf) + buffer_size()) * kNumBuffer;
    auto *mapped = mmap(nullptr, buf_ring_size, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
    if (mapped == MAP_FAILED) {
      fprintf(stderr, "buf_ring mmap: %s\n", strerror(errno));
      return -1;
    }
    buf_ring = static_cast<io_uring_buf_ring *>(mapped);
    io_uring_buf_ring_init(buf_ring);

    io_uring_buf_reg reg{.ring_addr = reinterpret_cast<unsigned long>(buf_ring),
                         .ring_entries = kNumBuffer,
                         .bgid = 0,
                         .flags = 0,
                         .resv = {0}};
    buffer_base = reinterpret_cast<unsigned char *>(buf_ring) +
                  sizeof(io_uring_buf) * kNumBuffer;

    int ret = io_uring_register_buf_ring(&ring, &reg, 0);
    if (ret) {
      fprintf(stderr, "buf_ring init failed: %s\n", strerror(-ret));
      return ret;
    }

    for (int i = 0; i < kNumBuffer; i++){
      io_uring_buf_ring_add(buf_ring, get_buffer(i), buffer_size(), i,
                            io_uring_buf_ring_mask(kNumBuffer), i);
    }
    io_uring_buf_ring_advance(buf_ring, kNumBuffer);
    return 0;
  }
};
} // namespace uring
