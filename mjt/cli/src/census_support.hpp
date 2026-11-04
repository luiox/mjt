/// @file        census_support.hpp
/// @brief       CLI census 读面公共件：一次性分析库 + 错误换型（cmd_info 与
///              cmd_classes 共链；纯机械提取，无独立逻辑）。
///
/// 数据面契约与 cmd_info.cpp 文件头同文：census 投影（censusContainer /
/// censusClasses / censusMembers，事实源是经 ingestIntoAnalysisDb 落库的
/// AnalysisDb）。分析库是进程临时目录的一次性文件，guard 析构即清理（含
/// -wal/-shm）。
#pragma once

#include <filesystem>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#define MJT_CLI_GETPID _getpid
#else
#include <unistd.h>
#define MJT_CLI_GETPID getpid
#endif

#include "mjbase/project/analysis_db.hpp"
#include "shared/i18n_errors.hpp"
#include "workspace/analysis_census.hpp"
#include "workspace/analysis_ingest.hpp"
#include "workspace/workspace_types.hpp"

namespace mjt::cli {

/// Utf8String/UTF-8 std::string → 借用视图（与 workspace 测试同款构造）。
inline ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// census 读面用的一次性分析库文件路径：临时目录 + 进程 id + 单调序号，进程内唯一。
inline std::string tempDbPath()
{
    static unsigned long long sequence = 0;
    const auto path = std::filesystem::temp_directory_path() /
                      ("mjt-cli-" + std::to_string(MJT_CLI_GETPID()) + "-" +
                       std::to_string(++sequence) + ".mjdb");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

/// RAII：分析库一次性文件清理（主文件 + WAL/SHM 伴生，失败静默）。
class TempDbGuard
{
public:
    explicit TempDbGuard(std::string path)
        : path_(std::move(path))
    {}
    ~TempDbGuard()
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        std::filesystem::remove(path_ + "-wal", ec);
        std::filesystem::remove(path_ + "-shm", ec);
    }
    TempDbGuard(const TempDbGuard&)            = delete;
    TempDbGuard& operator=(const TempDbGuard&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

/// AnalysisDb 错误（Utf8String message）→ workspace 错误值（i18n 模板承载）。
inline workspace::WorkspaceErrorInfo dbError(const mjbase::project::AnalysisDbError& cause)
{
    workspace::WorkspaceErrorInfo info;
    info.code    = workspace::WorkspaceError::Internal;
    info.message = shared::trfStr("mjt.cli.err.info_db", {cause.message.to_std_string()});
    return info;
}

/// 导入桥错误 → workspace 错误值（message 原样透传，已是呈现面文案）。
inline workspace::WorkspaceErrorInfo ingestError(const workspace::AnalysisIngestError& cause)
{
    workspace::WorkspaceErrorInfo info;
    info.code    = workspace::WorkspaceError::Internal;
    info.message = cause.message;
    return info;
}

/// census 投影错误 → workspace 错误值（message 原样透传）。
inline workspace::WorkspaceErrorInfo censusError(const workspace::CensusExportError& cause)
{
    workspace::WorkspaceErrorInfo info;
    info.code    = workspace::WorkspaceError::Internal;
    info.message = cause.message;
    return info;
}

/// AnalysisDb 查询/导入 Result 的错误通道换型（AnalysisDbError → 错误值）。
inline ca::Result<mjbase::project::DbContainerCensus, workspace::WorkspaceErrorInfo>
mapCensusError(ca::Result<mjbase::project::DbContainerCensus, mjbase::project::AnalysisDbError>&&
                   value)
{
    if (value.is_err()) return ca::Err(dbError(std::move(value).unwrap_err()));
    return ca::Ok(std::move(value).unwrap());
}

inline ca::Result<std::vector<mjbase::project::DbClassCensusRow>, workspace::WorkspaceErrorInfo>
mapClassRowsError(
    ca::Result<std::vector<mjbase::project::DbClassCensusRow>, mjbase::project::AnalysisDbError>&&
        value)
{
    if (value.is_err()) return ca::Err(dbError(std::move(value).unwrap_err()));
    return ca::Ok(std::move(value).unwrap());
}

inline ca::Result<std::vector<mjbase::project::DbMemberCensusRow>, workspace::WorkspaceErrorInfo>
mapMemberRowsError(
    ca::Result<std::vector<mjbase::project::DbMemberCensusRow>, mjbase::project::AnalysisDbError>&&
        value)
{
    if (value.is_err()) return ca::Err(dbError(std::move(value).unwrap_err()));
    return ca::Ok(std::move(value).unwrap());
}

}   // namespace mjt::cli
