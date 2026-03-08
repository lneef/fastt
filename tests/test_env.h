#pragma once

#include "util.h"

#include <gtest/gtest.h>
#include <rte_eal.h>

class DpdkEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    const char *argv[] = {"-l 0", "--no-huge"};
    int argc = sizeof(argv) / sizeof(argv[0]);
    int ret = rte_eal_init(argc, const_cast<char **>(argv));
    ASSERT_GE(ret, 0) << "Failed to initialize DPDK EAL";
    init_timing();
  }

  void TearDown() override{
      rte_eal_cleanup();
  }
};
