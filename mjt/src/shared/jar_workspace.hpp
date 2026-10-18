#pragma once

#include "mjbase/workspace/workspace.hpp"

namespace mjt {

///@brief mjt 的 Workspace 具体实现（workspace_decompose_plan 阶段三）。
///
///mjt 当前无扩展语义，直接继承 mjbase 基础实现；业务语义（若有）在 mjt 侧
///落位，不进 mjbase IWorkspace。所有 mjt 调用点面向 `IWorkspace` 编程，
///实例化点统一使用本类。
class JarWorkspace : public mjbase::workspace::WorkspaceBase
{
public:
    using WorkspaceBase::WorkspaceBase;
};

}   //namespace mjt
