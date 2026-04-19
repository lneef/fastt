#include "bench.h"
#include <cstdint>
#include <cstdio>
#include <rte_cycles.h>
#include <rte_eal.h>

void run() {
  bench::storage ste;
  bench::prepare(ste, 64, bench::kStoreSize);
  std::array<char, 128> buf;
  uint64_t res = 0;
  std::mt19937 rng;
  std::uniform_int_distribution<int64_t> dist{0, bench::kStoreSize};

  auto dur = rte_get_timer_hz() * 30;
  auto now = rte_get_timer_cycles();
  auto end = now + dur;
  uint64_t ops = 0;
  while (now < end) {
    auto key = dist(rng);
    auto it = ste.find(key);
    if (it != ste.end()) 
      std::memcpy(buf.data(), it->second.data(), it->second.size());
    
    now = rte_get_timer_cycles();
    ++ops;
  }

  printf("%f\n", static_cast<double>(ops) /
                     (static_cast<double>(dur) / rte_get_timer_hz()));
  printf("%lu\n", res);
}

int main(int argc, char** argv) {
  rte_eal_init(argc,  argv);  
  run();
  rte_eal_cleanup();
  return 0;
}
