#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

#include "service.hpp"
#include "shared/decompiler.hpp"

namespace mjt::mcp::detail {

struct DecompilerOptions final
{
    DecompilerKind            kind = DecompilerKind::Cfr;
    std::string               decompilerJar;
    std::string               javaProgram;
    std::chrono::milliseconds timeout{120000};
    ca::i32                   maxMemoryMiB   = 1024;
    ca::usize                 maxSourceBytes = 2 * 1024 * 1024;
};

struct DecompiledSource final
{
    std::string internalName;
    std::string backend;
    std::string decompilerJar;
    std::string source;
};

class ScopedTemporaryDirectory final
{
public:
    explicit ScopedTemporaryDirectory(std::filesystem::path path);
    ~ScopedTemporaryDirectory();

    const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path path_;
};

ca::StatusOr<std::filesystem::path> createDecompilerTemporaryDirectory();
ca::Status parseDecompilerSettings(const Json& arguments,
                                   const DecompilerSessionSettings& current,
                                   DecompilerSessionSettings& output);
ca::Status parseDecompilerOptions(const Json& arguments,
                                  const DecompilerSessionSettings& settings,
                                  DecompilerOptions& options);
Json decompilerSettingsJson(const DecompilerSessionSettings& settings);
Json decompilerAvailabilityList(const DecompilerSessionSettings& settings);
ca::StatusOr<DecompiledSource> decompileSnapshotClass(
    const std::filesystem::path& snapshot,
    const std::string& internalName,
    const DecompilerOptions& options,
    const std::function<bool()>& cancellationRequested = {});
ca::StatusOr<DecompiledSource> decompileWorkspaceClass(
    mjbase::workspace::IWorkspace& workspace,
    const std::string& className,
    const DecompilerOptions& options);
Json decompiledSourceJson(const DecompiledSource& result, const char* sourceScope);

}   //namespace mjt::mcp::detail
