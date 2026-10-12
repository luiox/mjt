#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief Propagate constants stored in local slots inside straight-line code.
///
///Usage scenario: undo local-variable based constant hiding, then leave simpler
///instruction streams for arithmetic and expression simplifiers.
///
///Before:
///@code
///iconst_5
///istore_1
///iload_1
///ireturn
///@endcode
///After:
///@code
///iconst_5
///istore_1
///iconst_5
///ireturn
///@endcode
///
///Flags: suitable for optimize/deobfuscate after basic control-flow cleanup. It
///is not in `default-optimize` until branch/frame-sensitive coverage is wider.
///
///Risk/boundary: propagation is deliberately local. Labels, branches, switch,
///`IINC`, return, and throw
///clear the tracked constants. The pass does not currently
///perform CFG joins or frame-aware
///propagation.
///Explicit wide/reference load forms and slot aliasing are outside the supported
///shape unless they are represented by the narrow shorthand opcodes already recognized by the
///implementation.
///
///@par 中文契约
///输入仅限直线 method tree；遇到
///label、frame、分支、switch、return、throw 或未知 slot
///
///时清空状态并保持该段不变，不做跨基本块传播。替换 load 才算 changed，返回
///
///`PreservedAnalyses::none`；无替换或空方法返回 `all`。clone 节点由 instruction list
///
///接管，失败不产生部分修改。当前为 opt-in，需补分支合流、roundtrip 和 JVM 证据后再评估默认链。


///@par 标签
///- `scope:method`
///- `action:propagate`
///- `stability:opt-in`
class LocalConstantPropagation : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};
}   //namespace mjt::base
