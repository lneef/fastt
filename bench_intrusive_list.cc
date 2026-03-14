#include <cstdint>
#include <cstdio>
#include <memory>
#include <rte_cycles.h>
#include <rte_eal.h>

#include "util.h"

struct element {
  list_hook link;
  int counter = 0;
};

int main(int argc, char **argv) {
  rte_eal_init(argc, argv);

  constexpr int N = 128;
  constexpr int ITERS = 1'000'000;

  std::unique_ptr<element> elems[N];
  intrusive_list_t<element> list;

  for (auto &e : elems) {
    e = std::make_unique<element>();
    list.push_back(*e);
  }

  // warmup
  for (auto &e : list)
    e.counter++;

  uint64_t best = UINT64_MAX;

  for (int i = 0; i < ITERS; i++) {
    uint64_t start = rte_rdtsc();
    for (auto &e : list)
      e.counter++;
    uint64_t end = rte_rdtsc();
    uint64_t elapsed = end - start;
    if (elapsed < best)
      best = elapsed;
  }

  printf("intrusive list iterate 128 elements: %lu cycles (best of %d)\n", best,
         ITERS);
  printf("  %.1f cycles/element\n", (double)best / N);

  rte_eal_cleanup();
}
