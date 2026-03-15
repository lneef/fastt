#include "slab_allocator.h"
#include "transport/protocol.h"
#include <chrono>
#include <cstdio>
#include <vector>

int main() {
  slab_allocator alloc;

  constexpr size_t N = 100000;
  std::vector<mbuf *> chunks(N);

  // Allocate N chunks
  auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < N; ++i) {
    chunks[i] = alloc.alloc_default(64);
  }

  auto t1 = std::chrono::steady_clock::now();
  for(size_t i = 0; i< N; ++i){
      auto *ft = chunks[i]->prepend<protocol::ft_header>();
      ft->ack = {0};
      ft->ackframe = 1;
      ft->crd = 1;
      ft->sack = 0;
      ft->seq = {0};
  }

  auto t2 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < N; ++i) {
    alloc.free_mbuf(chunks[i]);
  }

  auto t3 = std::chrono::steady_clock::now();

  for (size_t i = 0; i < N; ++i) {
    chunks[i] = alloc.alloc_default(64);
  }

  auto t4 = std::chrono::steady_clock::now();

  auto alloc_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  auto set_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
  auto free_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
  auto as_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t4 - t3).count();

  std::printf("allocated and freed %zu chunks\n", N);
  std::printf("alloc: %ld ns total, %ld ns/op\n", alloc_ns, alloc_ns / (long)N);
  std::printf("access:  %ld ns total, %ld ns/op\n", set_ns, set_ns / (long)N);
  std::printf("free:  %ld ns total, %ld ns/op\n", free_ns, free_ns / (long)N);
  std::printf("alloc second:  %ld ns total, %ld ns/op\n", as_ns, as_ns / (long)N);


  // Second pass: re-allocate from freed slabs and free again
  for (size_t i = 0; i < N; ++i) {
    chunks[i] = alloc.alloc_default(64);
  }
  for (size_t i = 0; i < N; ++i) {
    alloc.free_mbuf(chunks[i]);
  }
  std::printf("second alloc+free pass complete\n");

  return 0;
}
