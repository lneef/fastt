#include "bench.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

int main(int argc, char *argv[]) {
  unsigned val_len = 8;
  if (argc > 1)
    val_len = std::atoi(argv[1]);

  bench::storage store;
  bench::prepare(store, val_len);

  std::mt19937_64 rng{42};
  std::uniform_int_distribution<int64_t> dist(0, bench::kStoreSize - 1);

  // warm up
  for (int i = 0; i < 1000; ++i)
    (void)store.find(dist(rng));

  uint64_t count = 0;
  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::seconds(1);

  while (std::chrono::steady_clock::now() < deadline) {
    store.find(dist(rng));
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
