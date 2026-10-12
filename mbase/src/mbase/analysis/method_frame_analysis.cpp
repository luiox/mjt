#include "mbase/analysis/method_frame_analysis.hpp"

namespace mjt::base {

//FrameAnalysis<InterpreterT, ValueT>::run 是模板，定义在头文件（含自管容量的
//TransientCapacityScope）。本 TU 仅提供非模板的注册器与工厂。

void registerBuiltinMethodAnalyses(MethodAnalysisManager& mam)
{
    //registerAnalysis 自身幂等（重复注册直接返回），此处无需再判 isRegistered。
    //目前仅 OriginalSourceFrameAnalysis 是多消费者、需跨 pass 缓存的帧分析；其余 analyzer
    //族要么单消费者、要么在 pass 内定点循环重算（缓存会读脏），故不注册进 AM。
    mam.registerAnalysis<OriginalSourceFrameAnalysis>();
}

MethodAnalysisManager makeMethodAnalysisManager(ca::str::Utf8StringArena& arena)
{
    MethodAnalysisManager mam(arena);
    registerBuiltinMethodAnalyses(mam);
    return mam;   //AnalysisManager 可移动（仅 map+指针），NRVO/move，cache 此时为空不悬垂。
}

}   //namespace mjt::base
