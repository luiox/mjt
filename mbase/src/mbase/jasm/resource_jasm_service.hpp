#pragma once

#include "jvm_jasm_service.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/spi/pass_pipeline.hpp"

#include <string>

namespace mjt::base::jasm {

///@brief 将完整 class Jasm 操作事务式接入 ResourceContainer。
///
///汇编仅在名称校验和候选工作区验证全部成功后替换 ClassResource；成功替换会清理
///当前 ResourceContainer 的 GlobalAnalysisManager 缓存，失败不会修改资源内容。
class ResourceJasmService final
{
public:
    ResourceJasmService(ResourceContainer& container, GlobalAnalysisManager& analysisManager)
        : container_(&container)
        , analysisManager_(&analysisManager)
    {}

    ///@brief 按 URI 反汇编 ClassResource。
    Result<JvmJasmDisassembly> disassemble(const std::string& uri) const;

    ///@brief 按 URI 事务式汇编并替换已有 ClassResource。
    Result<JvmJasmClass> assemble(const std::string& uri, ca::str::Utf16StringRef sourceName,
                                  ca::str::Utf16StringRef source, JvmJasmContext context = {});

private:
    ResourceContainer*     container_;
    GlobalAnalysisManager* analysisManager_;
    JvmJasmService         jasm_;
};

}   //namespace mjt::base::jasm
