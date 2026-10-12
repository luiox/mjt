#include "jblw/jblw.hpp"

#include <gtest/gtest.h>

#include <string_view>

TEST(LibJblwEntryTest, ReportsPinnedUpstreamRevision)
{
    EXPECT_EQ(std::string_view(jblw::get_upstream_revision()),
              "6fb9d1a828a6c39cd456bbd51beebcb67c3f55aa");
    EXPECT_FALSE(std::string_view(jblw::get_lib_version()).empty());
}
