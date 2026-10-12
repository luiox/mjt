#pragma once

#include "jhasm/ast/specific.hpp"
#include "jhasm/analysis/inheritance_checker.hpp"
#include "jhasm/error/result.hpp"

#include <jblw/classfile/model.hpp>
#include <jblw/version/java_version.hpp>

#include <vector>

namespace jhasm::compile {

struct JvmCompilerOptions final
{
    jblw::version::JavaVersion version      = jblw::version::JavaVersion::V17;
    ca::i32                    maxStackHint = 128;
    ///@brief 非拥有指针；非空时用于 StackMap frame 的 common-super 计算。
    const analysis::InheritanceChecker* inheritance = nullptr;
};

struct JvmCompileResult final
{
    jblw::classfile::ClassFileViewPtr classFile;
    std::vector<ca::u8>               bytes;
};

///@brief 将处理后的 JVM AST 编译为 BLW class 模型和 classfile bytes。
class JvmCompiler final
{
public:
    explicit JvmCompiler(JvmCompilerOptions options = {})
        : options_(options)
    {}
    error::Result<JvmCompileResult> compile(const std::vector<ast::ElementPtr>& input) const;
    error::Result<JvmCompileResult> compile(const ast::Class& clazz) const;

private:
    JvmCompilerOptions options_;
};

}   //namespace jhasm::compile
