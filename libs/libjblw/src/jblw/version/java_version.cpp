#include "java_version.hpp"

namespace jblw::version {

const JavaVersion JavaVersion::V1  = JavaVersion::jdkVersion(1);
const JavaVersion JavaVersion::V2  = JavaVersion::jdkVersion(2);
const JavaVersion JavaVersion::V3  = JavaVersion::jdkVersion(3);
const JavaVersion JavaVersion::V4  = JavaVersion::jdkVersion(4);
const JavaVersion JavaVersion::V5  = JavaVersion::jdkVersion(5);
const JavaVersion JavaVersion::V6  = JavaVersion::jdkVersion(6);
const JavaVersion JavaVersion::V7  = JavaVersion::jdkVersion(7);
const JavaVersion JavaVersion::V8  = JavaVersion::jdkVersion(8);
const JavaVersion JavaVersion::V9  = JavaVersion::jdkVersion(9);
const JavaVersion JavaVersion::V10 = JavaVersion::jdkVersion(10);
const JavaVersion JavaVersion::V11 = JavaVersion::jdkVersion(11);
const JavaVersion JavaVersion::V12 = JavaVersion::jdkVersion(12);
const JavaVersion JavaVersion::V14 = JavaVersion::jdkVersion(14);
const JavaVersion JavaVersion::V15 = JavaVersion::jdkVersion(15);
const JavaVersion JavaVersion::V16 = JavaVersion::jdkVersion(16);
const JavaVersion JavaVersion::V17 = JavaVersion::jdkVersion(17);
const JavaVersion JavaVersion::V18 = JavaVersion::jdkVersion(18);
const JavaVersion JavaVersion::V19 = JavaVersion::jdkVersion(19);
const JavaVersion JavaVersion::V20 = JavaVersion::jdkVersion(20);
const JavaVersion JavaVersion::V21 = JavaVersion::jdkVersion(21);
const JavaVersion JavaVersion::V22 = JavaVersion::jdkVersion(22);

ca::i32 JavaVersion::pack() const noexcept
{
    const auto major = static_cast<ca::u32>(majorVersion_);
    const auto minor = static_cast<ca::u32>(minorVersion_);
    return static_cast<ca::i32>(major | (minor << 16U));
}

JavaVersion JavaVersion::withPreviewFeatures() const noexcept
{
    if (arePreviewFeaturesEnabled()) return *this;
    return classVersion(majorVersion_, PREVIEW_FEATURES);
}

JavaVersion JavaVersion::dropPreviewFeatures() const noexcept
{
    if (!arePreviewFeaturesEnabled()) return *this;
    return classVersion(majorVersion_);
}

}   //namespace jblw::version
