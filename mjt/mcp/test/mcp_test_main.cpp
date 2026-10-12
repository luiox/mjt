#include <gtest/gtest.h>

#include "mcp_test_support.hpp"
#include "test_helper.hpp"

int main(int argc, char** argv)
{
    const auto fakeResult = mjt::mcp::runFakeDecompilerProcess(argc, argv);
    if (fakeResult >= 0) return fakeResult;
    test::setup("mjt");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
