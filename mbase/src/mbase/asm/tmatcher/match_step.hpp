#pragma once

#include <string>
#include <vector>

#include "jbytecode/tree/insn_list.hpp"

#include "mbase/asm/tmatcher/match_state.hpp"
#include "mbase/asm/tmatcher/match_tracer.hpp"

namespace mjt::tmatcher {

///指令匹配 Step 抽象基类。
///
///设计要点（对齐 issue ）：
///- 核心方法 match 返回候选状态列表（vector<MatchState>），空列表表示失败。
///- 这与旧的线性 tryMatch(now)->bool 不同：返回多候选状态支撑量词（repeat/any 等）
///的回溯降级，是贪婪优先 + 回溯语义的基础。
///- 状态不可变（MatchState），每个分支返回独立快照，天然支持组合子嵌套。
///
///name 供 MatchTracer 调试输出标识当前 step。
class IMatchStep
{
public:
    virtual ~IMatchStep() = default;

    ///在 list 上从 state.cursor 开始尝试匹配。
    ///返回所有可达的候选后继状态（按 cursor 降序排列，贪婪优先）。
    ///空向量表示匹配失败。
    virtual std::vector<MatchState> match(jbytecode::tree::InsnList&     list,
                                          const MatchState&               state,
                                          MatchTracer&                    tracer,
                                          int                             depth) = 0;

    ///调试标识，例如 "opcode(IADD)" / "seq" / "repeat(min=1,max=3)"。
    virtual std::string name() const = 0;
};

}   //namespace mjt::tmatcher
