#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#include <pthread.h>
#include <sched.h>

#ifdef __x86_64__

static uint64_t freq = 0;
__inline uint64_t rdtsc() {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

__inline uint64_t rdtsc_precise() {
  uint32_t lo, hi, aux;
  __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
  return ((uint64_t)hi << 32) | lo;
}

inline void init_tsc(){
    auto now = std::chrono::steady_clock::now();
    auto tsc_now = rdtsc_precise();
    auto start = now;
    auto end = now + std::chrono::milliseconds(1000);
    while(start < end)
        start = std::chrono::steady_clock::now();
    auto tsc_end = rdtsc_precise();

    freq = (tsc_end - tsc_now);
}
#include <cpuid.h>
__inline uint64_t get_tsc_freq() {
    return freq;
}

#else
#include <arm_acle.h>

inline void init_tsc(){}

__inline uint64_t get_tsc_freq() {
  uint64_t freq;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq;
}

__inline uint64_t rdtsc() {
  uint64_t val;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
  return val;
}

__inline uint64_t rdtsc_precise() {
  asm volatile("isb" : : : "memory");
  return rdtsc();
}
#endif // __x86_64__

inline int set_thread_affinity(pthread_t t, uint16_t core) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);
  return pthread_setaffinity_np(t, sizeof(cpuset), &cpuset);
}
