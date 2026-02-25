#include "test_env.h"

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new DpdkEnvironment);
    return RUN_ALL_TESTS();
}

