#pragma once

#include "frame.hpp"
#include "inheritance_checker.hpp"
#include "jhasm/error/result.hpp"

#include <libca/core/datatype.hpp>

#include <optional>
#include <vector>

namespace jhasm::analysis {

struct MethodAnalysis final
{
    ca::str::Utf8String               name;
    ca::str::Utf8String               descriptor;
    std::vector<std::optional<Frame>> frames;
};

struct JvmAnalysisResult final
{
    std::vector<MethodAnalysis> methods;
};

///@brief 对 classfile bytes 执行 JVM 类型数据流分析并返回 owning frame 快照。
class JvmAnalyzer final
{
public:
    explicit JvmAnalyzer(
        const InheritanceChecker& inheritance = EmptyInheritanceChecker::instance()) noexcept
        : inheritance_(&inheritance)
    {}

    error::Result<JvmAnalysisResult> analyze(const std::vector<ca::u8>& classBytes) const;

private:
    const InheritanceChecker* inheritance_;
};

}   //namespace jhasm::analysis
