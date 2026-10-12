#include "jblw/version/java_version.hpp"

#include <gtest/gtest.h>

using jblw::version::JavaVersion;

TEST(JavaVersionTest, ConvertsBetweenJdkAndClassVersions)
{
    EXPECT_EQ(JavaVersion::V8, JavaVersion::jdkVersion(8));
    EXPECT_EQ(52, JavaVersion::V8.majorVersion());
    EXPECT_EQ(0, JavaVersion::V8.minorVersion());
    EXPECT_EQ(8, JavaVersion::V8.jdkVersion());
    EXPECT_EQ(52, JavaVersion::V8.pack());
}

TEST(JavaVersionTest, PreviewRoundTripPreservesMajorVersion)
{
    const auto preview = JavaVersion::V21.withPreviewFeatures();
    EXPECT_TRUE(preview.arePreviewFeaturesEnabled());
    EXPECT_EQ(65, preview.majorVersion());
    EXPECT_EQ(0xFFFF, preview.minorVersion());
    EXPECT_EQ(static_cast<ca::i32>(0xFFFF0041U), preview.pack());
    EXPECT_EQ(JavaVersion::V21, preview.dropPreviewFeatures());
    EXPECT_EQ(preview, preview.withPreviewFeatures());
}

TEST(JavaVersionTest, KeepsUpstreamNamedVersionSet)
{
    EXPECT_EQ(56, JavaVersion::V12.majorVersion());
    EXPECT_EQ(58, JavaVersion::V14.majorVersion());
    EXPECT_EQ(66, JavaVersion::V22.majorVersion());
}
