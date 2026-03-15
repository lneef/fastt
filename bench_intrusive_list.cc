#include <cstdint>
#include <cstdio>
#include <generic/rte_cycles.h>
#include <memory>
#include <rte_cycles.h>
#include <rte_eal.h>

#include "util.h"

struct element {
  list_hook link;

  char pad[4096];
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

  uint64_t best = UINT64_MAX, worst = 0;
  uint64_t total = 0;

  for (int i = 0; i < ITERS; i++) {
    uint64_t start = rte_get_timer_cycles();
    uint64_t dep = 0;
    for (auto &e : list) {
        e.counter += dep;
        dep = e.counter;
    }
    uint64_t end = rte_get_timer_cycles();
    total += dep;
    uint64_t elapsed = end - start;
    if (elapsed < best)
      best = elapsed;
    if(elapsed > worst)
        worst = elapsed;
  }

  printf("intrusive list iterate 128 elements: %lu cycles (best of %d)\n", best,
         ITERS);
  printf("  %.1f cycles/element\n", (double)best / N);

  printf("  %.1f cycles/element\n", (double)worst / (rte_get_timer_hz() / 1e6));
  printf("%lu\n", total);

  rte_eal_cleanup();
}
