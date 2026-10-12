#pragma once

#include <libca/core/datatype.hpp>

namespace jblw::version {

///@brief JVM class 文件版本值，对齐 BLW 的 JavaVersion。
///
///类型为不可变值对象；preview 状态由 minor version `0xFFFF` 表示。
class JavaVersion final
{
public:
    static constexpr ca::i32 CLASS_VERSION_OFFSET = 44;

    static const JavaVersion V1;
    static const JavaVersion V2;
    static const JavaVersion V3;
    static const JavaVersion V4;
    static const JavaVersion V5;
    static const JavaVersion V6;
    static const JavaVersion V7;
    static const JavaVersion V8;
    static const JavaVersion V9;
    static const JavaVersion V10;
    static const JavaVersion V11;
    static const JavaVersion V12;
    static const JavaVersion V14;
    static const JavaVersion V15;
    static const JavaVersion V16;
    static const JavaVersion V17;
    static const JavaVersion V18;
    static const JavaVersion V19;
    static const JavaVersion V20;
    static const JavaVersion V21;
    static const JavaVersion V22;

    ///@brief 从 class major/minor version 创建版本值。
    static constexpr JavaVersion classVersion(ca::i32 majorVersion,
                                              ca::i32 minorVersion = 0) noexcept
    {
        return JavaVersion(majorVersion, minorVersion);
    }

    ///@brief 从 JDK feature version 创建普通 class 版本值。
    static constexpr JavaVersion jdkVersion(ca::i32 version) noexcept
    {
        return classVersion(version + CLASS_VERSION_OFFSET);
    }

    ///@brief 返回 class major version。
    constexpr ca::i32 majorVersion() const noexcept { return majorVersion_; }

    ///@brief 返回 class minor version。
    constexpr ca::i32 minorVersion() const noexcept { return minorVersion_; }

    ///@brief 按 BLW 的低 16 位 major、高 16 位 minor 规则打包。
    ca::i32 pack() const noexcept;

    ///@brief 判断 minor version 是否启用了 preview features。
    constexpr bool arePreviewFeaturesEnabled() const noexcept
    {
        return minorVersion_ == PREVIEW_FEATURES;
    }

    ///@brief 返回启用 preview features 的版本副本。
    JavaVersion withPreviewFeatures() const noexcept;

    ///@brief 返回移除 preview features 的版本副本。
    JavaVersion dropPreviewFeatures() const noexcept;

    ///@brief 返回对应的 JDK feature version。
    constexpr ca::i32 jdkVersion() const noexcept { return majorVersion_ - CLASS_VERSION_OFFSET; }

    ///@brief 比较 major/minor version 是否相同。
    friend constexpr bool operator==(JavaVersion lhs, JavaVersion rhs) noexcept
    {
        return lhs.majorVersion_ == rhs.majorVersion_ && lhs.minorVersion_ == rhs.minorVersion_;
    }

    ///@brief 比较 major/minor version 是否不同。
    friend constexpr bool operator!=(JavaVersion lhs, JavaVersion rhs) noexcept
    {
        return !(lhs == rhs);
    }

private:
    static constexpr ca::i32 PREVIEW_FEATURES = 0xFFFF;

    constexpr JavaVersion(ca::i32 majorVersion, ca::i32 minorVersion) noexcept
        : majorVersion_(majorVersion)
        , minorVersion_(minorVersion)
    {}

    ca::i32 majorVersion_;
    ca::i32 minorVersion_;
};

}   //namespace jblw::version
