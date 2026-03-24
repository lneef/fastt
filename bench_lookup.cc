#include "bench.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

int main(int argc, char *argv[]) {
  unsigned val_len = 8;
  if (argc > 1)
    val_len = std::atoi(argv[1]);

  bench::storage store;
  bench::prepare(store, val_len);

  std::mt19937_64 rng{42};
  std::uniform_int_distribution<int64_t> dist(0, bench::kStoreSize - 1);

  std::vector<char> buf(val_len);

  // warm up
  for (int i = 0; i < 1000; ++i) {
    auto it = store.find(dist(rng));
    std::memcpy(buf.data(), it->second.data(), val_len);
  }

  uint64_t count = 0;
  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::seconds(1);

  while (std::chrono::steady_clock::now() < deadline) {
    auto it = store.find(dist(rng));
    std::memcpy(buf.data(), it->second.data(), val_len);
    asm volatile("" : : "m"(buf) : "memory");
    ++count;
  }

  auto elapsed = std::chrono::steady_clock::now() - start;
  double secs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() /
      1e9;

  std::printf("%lu lookups in %.3f s  (%.2f M lookups/s)\n", count, secs,
              count / secs / 1e6);
  return 0;
}
