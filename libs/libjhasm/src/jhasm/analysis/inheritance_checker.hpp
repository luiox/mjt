#pragma once

#include <libca/str/utf8_string.hpp>

namespace jhasm::analysis {

///@brief 为 JVM 类型分析提供不依赖类加载器的继承关系查询。
class InheritanceChecker
{
public:
    virtual ~InheritanceChecker() = default;

    virtual bool                isSubclassOf(ca::str::Utf8StringRef child,
                                             ca::str::Utf8StringRef parent) const     = 0;
    virtual ca::str::Utf8String commonSuperclass(ca::str::Utf8StringRef first,
                                                 ca::str::Utf8StringRef second) const = 0;
};

///@brief 无 classpath 时使用的保守继承查询，与上游 EmptyInheritanceChecker 对齐。
class EmptyInheritanceChecker final : public InheritanceChecker
{
public:
    static const EmptyInheritanceChecker& instance() noexcept;

    bool isSubclassOf(ca::str::Utf8StringRef child, ca::str::Utf8StringRef parent) const override;
    ca::str::Utf8String commonSuperclass(ca::str::Utf8StringRef first,
                                         ca::str::Utf8StringRef second) const override;
};

}   //namespace jhasm::analysis
