#include <gtest/gtest.h>

#include "test_helper.hpp"

int main(int argc, char** argv)
{
    test::setup("mjt");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
