#pragma once

#include "jbytecode/jbytecode.hpp"
//Utf8 string arena for global ownership
#include "libca/str/utf8_string_arena.hpp"
//canonical pass 基类 + PassManager + AnalysisManager 别名
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

///@brief 作业级根上下文：唯一持有 Utf8StringArena，活到整个 jar 作业结束。
///
///对标 dev_plan Stage A2 的「单 arena 贯穿作业」。各级 AnalysisManager 构造时借用此
///arena（经 am.arena 暴露给 pass）。pass 不再直接接收 context —— 新框架 pass 签名是
///run(IRUnit&, AnalysisManager&)（见 pass-framework-design.md §3/§6）。
///
///@warning 非线程安全。
class AsmGlobalContext
{
public:
    ca::str::Utf8StringArena&       stringArena() { return arena_; }
    const ca::str::Utf8StringArena& stringArena() const { return arena_; }

private:
    //-- arena 死亡点声明（memory-governance-plan §5-P1③）--
    //本成员持有作业私有的 Utf8StringArena（全作业唯一，见类注释）：经
    //stringArena 借给各级 AnalysisManager，pass intern 的字符串（写回
    //ClassNode 的常量、分析缓存里的类/成员名视图）全部指向它。
    //视图寿命 = 本对象寿命；死亡点：随持有者析构——本类总是作为作业栈帧上的
    //局部对象存在（pass_manager.cpp:29 的 GlobalPassManager::run、
    //cli_runner.cpp:410、workspace.cpp:3346、sdk/src/workspace.cpp:81 各持一个），
    //析构即整个 jar 作业结束，所有视图随之失效。
    //消费者不得把视图带过作业结束点；需跨期持有则 intern 进自己的 arena 或深拷贝。
    ca::str::Utf8StringArena arena_;
};

}   //namespace mjt::base
