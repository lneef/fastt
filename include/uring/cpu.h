#pragma once

#include <cassert>
#include <cpuid.h>
#include <cstdint>

#include <pthread.h>
#include <sched.h>

__inline uint64_t get_tsc_freq() {
  uint32_t eax, ebx, ecx, edx;
  __cpuid(0x15, eax, ebx, ecx, edx);
  assert(eax && ebx && ecx);
  return static_cast<uint64_t>(ecx * ebx) / eax;
}

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

inline int set_thread_affinity(pthread_t t, uint16_t core){
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(t, sizeof(cpuset), &cpuset);
}
