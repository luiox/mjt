#pragma once

#include <vector>

#include "mbase/spi/asm_pass.hpp"
#include "mbase/model/resource_container.hpp"
#include "jbytecode/jbytecode.hpp"

namespace mjt::base {

///@brief ASM 字节码单类编排入口：bytes → ClassNode → managers → ClassWriter → bytes。
///
///用新框架（PassManager 模板 + AnalysisManager）跑一个 class 的 pass 流水线。
///arena 由内部 AsmGlobalContext 持有并贯穿所有 pass（经各级 AnalysisManager 的
///am.arena 暴露）。CLI 的 ClassProcessor 已逐类回调，故此处按单类驱动即可；
///跨类（ResourceContainer 级）的 GlobalToClassPassAdaptor 见 pass_pipeline.hpp。
///
///@param classBytes 输入 class 字节
///@param classManager 可选类级 pass 编排器（nullptr 跳过）
///@param methodManager 可选方法级 pass 编排器（nullptr 跳过）
///@param classWriterFlags ClassWriter flags（COMPUTE_MAXS | COMPUTE_FRAMES，默认 0）
///@param typeResolver 可选 ICommonSuperClassProvider（COMPUTE_FRAMES 时用）
///@return 转换后的 class 字节
std::vector<ca::u8> runAsmPipeline(
    const std::vector<ca::u8>& classBytes, ClassPassManager* classManager,
    MethodPassManager* methodManager, int classWriterFlags = 0,
    jbytecode::core::ICommonSuperClassProvider* typeResolver = nullptr);

///@brief Jar 级字节码编排入口：容器 → GlobalPassManager → 写出。
///
///在 ResourceContainer 上跑全局流水线（含 GlobalToClassPassAdaptor 驱动的跨类编排）。
///arena 由内部 GlobalAnalysisManager 的 arena 贯穿整个作业。
///跨类的 passes（rename / 调用图 / 纯函数内联）在此载体上运行。
///
///@param container 资源容器（import 后传入，处理后由调用方 export）
///@param gpm 全局 PassManager（含嵌套的 Adaptor + class/method passes）
///@param globalContext 作业级上下文（持有独有的 arena）
///@param classWriterFlags ClassWriter flags（COMPUTE_MAXS | COMPUTE_FRAMES，默认 0）
///@param typeResolver 可选 ICommonSuperClassProvider
void runJarPipeline(ResourceContainer& container, GlobalPassManager& gpm,
                    AsmGlobalContext& globalContext, int classWriterFlags = 0,
                    jbytecode::core::ICommonSuperClassProvider* typeResolver = nullptr);

}   //namespace mjt::base
