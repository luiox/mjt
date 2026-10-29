#pragma once

//status_convert.hpp
//
//mjbase workspace 错误通道（WorkspaceErrorInfo 值语义）→ mjt 旧 ca::Status
//呈现通道的最小映射。mjt 内部仍以 ca::StatusOr 承载自身 API（反编译管线等
//不经 workspace 的路径），跨界点经本头转换，码值逐项对齐不静默收窄。

#include <string>

#include "libca/core/status.hpp"
#include "workspace/workspace_types.hpp"

namespace mjt::shared {

///WorkspaceError → ca::StatusCode（逐项对齐；Ok/Internal 兜底 INTERNAL）。
inline ca::StatusCode statusCodeOf(mjt::workspace::WorkspaceError code)
{
    switch (code) {
    case mjt::workspace::WorkspaceError::InvalidArgument:
        return ca::StatusCode::INVALID_ARGUMENT;
    case mjt::workspace::WorkspaceError::NotFound: return ca::StatusCode::NOT_FOUND;
    case mjt::workspace::WorkspaceError::AlreadyExists:
        return ca::StatusCode::ALREADY_EXISTS;
    case mjt::workspace::WorkspaceError::FailedPrecondition:
        return ca::StatusCode::FAILED_PRECONDITION;
    case mjt::workspace::WorkspaceError::PermissionDenied:
        return ca::StatusCode::PERMISSION_DENIED;
    case mjt::workspace::WorkspaceError::Aborted: return ca::StatusCode::ABORTED;
    case mjt::workspace::WorkspaceError::DataLoss: return ca::StatusCode::DATA_LOSS;
    case mjt::workspace::WorkspaceError::Unimplemented:
        return ca::StatusCode::UNIMPLEMENTED;
    case mjt::workspace::WorkspaceError::Ok:
    case mjt::workspace::WorkspaceError::Internal: break;
    }
    return ca::StatusCode::INTERNAL;
}

///WorkspaceErrorInfo → ca::Status（message 文本原样透传；OK 值映射回 ca::OkStatus，
///否则成功路径会被误判为失败——commit 成功即被当成失败上报）。
inline ca::Status toCaStatus(const mjt::workspace::WorkspaceErrorInfo& info)
{
    if (!info.is_err()) return ca::OkStatus();
    return ca::ErrStatus(statusCodeOf(info.code), info.message);
}

///ca::StatusCode → WorkspaceError（反向对齐；CANCELLED 并入 Aborted，
///DEADLINE_EXCEEDED/UNAVAILABLE 等无对应值的码兜底 Internal）。
inline mjt::workspace::WorkspaceError workspaceErrorOf(ca::StatusCode code)
{
    switch (code) {
    case ca::StatusCode::INVALID_ARGUMENT:
        return mjt::workspace::WorkspaceError::InvalidArgument;
    case ca::StatusCode::NOT_FOUND: return mjt::workspace::WorkspaceError::NotFound;
    case ca::StatusCode::ALREADY_EXISTS:
        return mjt::workspace::WorkspaceError::AlreadyExists;
    case ca::StatusCode::FAILED_PRECONDITION:
        return mjt::workspace::WorkspaceError::FailedPrecondition;
    case ca::StatusCode::PERMISSION_DENIED:
        return mjt::workspace::WorkspaceError::PermissionDenied;
    case ca::StatusCode::ABORTED:
    case ca::StatusCode::CANCELLED: return mjt::workspace::WorkspaceError::Aborted;
    case ca::StatusCode::DATA_LOSS: return mjt::workspace::WorkspaceError::DataLoss;
    case ca::StatusCode::UNIMPLEMENTED:
        return mjt::workspace::WorkspaceError::Unimplemented;
    case ca::StatusCode::OK:
    default: break;
    }
    return mjt::workspace::WorkspaceError::Internal;
}

///ca::Status → WorkspaceErrorInfo（message 文本原样透传；mjt 自有 API 的
///ca::Status 错误流回 mjbase 候选事务通道时使用。OK 值映射回默认 Ok info）。
inline mjt::workspace::WorkspaceErrorInfo fromCaStatus(const ca::Status& status)
{
    mjt::workspace::WorkspaceErrorInfo info;
    if (!status.is_err()) return info;
    info.code    = workspaceErrorOf(status.code());
    info.message = status.message();
    return info;
}

}   //namespace mjt::shared
