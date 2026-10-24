#include "project_session.hpp"

#include <atomic>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>
#include <vector>

#include "libca/str/utf8_string.hpp"

#include "jbc/core/reader.hpp"

#include "mjbase/project/analysis_project.hpp"
#include "mjbase/project/project_store.hpp"

#include "shared/status_convert.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mjt::mcp {

namespace {

namespace fs = std::filesystem;

using mjbase::project::AnalysisInput;
using mjbase::project::AnalysisProject;
using mjbase::project::AnalysisProjectError;
ca::str::Utf8StringRef utf8Ref(const std::string& value) noexcept
{
    return ca::str::Utf8StringRef::from_data(
        value.empty() ? nullptr : reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

///工程实体错误 → ca::Status（schema 违约按调用方参数问题回报，读不出按内部错误）。
ca::Status analysisProjectError(const AnalysisProjectError& error, const char* operation)
{
    const auto code = error.kind == mjbase::project::AnalysisProjectErrorKind::Io
                          ? ca::StatusCode::INTERNAL
                          : ca::StatusCode::INVALID_ARGUMENT;
    return ca::ErrStatus(code,
                         std::string(operation) + ": " + error.message.to_std_string());
}

///文件字节的 sha256 登记指纹（schema v2 裁决：工程登记一律 sha256 口径）。
ca::Status fileFingerprint(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return ca::ErrStatus(ca::StatusCode::NOT_FOUND, "open file failed: " + path.string());
    std::vector<char> bytes{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
    out = mjbase::project::sectionContentHash(
              reinterpret_cast<const ca::u8*>(bytes.data()), bytes.size())
              .to_std_string();
    return ca::OkStatus();
}

///进程内单调序号 + 进程 id：同进程多次开临时工程互不相撞，跨进程靠 pid 分散。
ca::u64 nextTemporarySequence()
{
    static std::atomic<ca::u64> sequence{0};
    return sequence.fetch_add(1);
}

ca::Status makeTemporaryProjectDir(std::string& out)
{
    std::error_code ec;
    const auto tempRoot = fs::temp_directory_path(ec);
    if (ec)
        return ca::ErrStatus(ca::StatusCode::INTERNAL,
                             "resolve temporary directory failed: " + ec.message());
#if defined(_WIN32)
    const auto pid = static_cast<ca::u64>(::GetCurrentProcessId());
#else
    const auto pid = static_cast<ca::u64>(::getpid());
#endif
    for (ca::u32 attempt = 0; attempt < 64; ++attempt) {
        const auto candidate =
            tempRoot / ("mjt-project-" + std::to_string(pid) + "-" +
                        std::to_string(nextTemporarySequence()));
        ec.clear();
        if (fs::create_directories(candidate, ec)) {
            out = candidate.string();
            return ca::OkStatus();
        }
        //目录已存在 = 序号撞名（理论不可达），换名重试；真实文件系统失败上浮。
        if (ec) {
            if (ec == std::errc::file_exists) continue;
            return ca::ErrStatus(ca::StatusCode::INTERNAL,
                                 "create temporary project directory failed: " + ec.message());
        }
    }
    return ca::ErrStatus(ca::StatusCode::INTERNAL,
                         "create temporary project directory failed: exhausted unique names");
}

///primary 容器登记：同 path 已登记时指纹不一致即重算更新（容器内容变化），
///一致为幂等 no-op；工程已绑定其他 primary 时拒绝——工程与主容器一一绑定，
///换主容器请另开工程（台账基线与容器登记的对应关系不随换绑漂移）。
ca::Status registerPrimaryInput(const fs::path&    bundleDir,
                                const std::string& jarPath,
                                const std::string& fingerprint)
{
    const auto projectFile = bundleDir / "project.json";
    auto loaded = mjbase::project::readAnalysisProjectFile(utf8Ref(projectFile.string()));
    if (loaded.is_err()) return analysisProjectError(std::move(loaded).unwrap_err(), "read project entity");
    auto project = std::move(loaded).unwrap();

    const auto pathKey = mjbase::project::copyLedgerText(jarPath);
    const auto fingerprintKey = mjbase::project::copyLedgerText(fingerprint);
    AnalysisInput input;
    input.path        = pathKey.clone();
    input.fingerprint = fingerprintKey.clone();
    input.role        = mjbase::project::AnalysisInputRole::Primary;

    if (const auto* existing = project.findInput(pathKey.ref())) {
        if (existing->fingerprint.ref().equals(fingerprintKey.ref())) return ca::OkStatus();
        if (project.removeInput(pathKey.ref()) != 1)
            return ca::ErrStatus(ca::StatusCode::INTERNAL,
                                 "register primary container: stale input could not be removed");
    }
    else {
        for (const auto& registered : project.inputs()) {
            if (registered.role != mjbase::project::AnalysisInputRole::Primary) continue;
            return ca::ErrStatus(
                ca::StatusCode::INVALID_ARGUMENT,
                "project already binds a different primary container: " +
                    registered.path.to_std_string() + "; create a new project instead");
        }
    }
    if (!project.addInput(std::move(input)))
        return ca::ErrStatus(ca::StatusCode::INTERNAL,
                             "register primary container: input rejected");
    auto written = mjbase::project::writeAnalysisProjectFile(
        utf8Ref(projectFile.string()), project);
    if (written.is_err()) return analysisProjectError(std::move(written).unwrap_err(), "write project entity");
    return ca::OkStatus();
}

///角色化容器登记（container_mount 挂点）：同 path 幂等，指纹变化即刷新
///（容器内容更新）；写入 inputs（role + 可选 parent 挂载父容器）。support
///挂载只进登记面，不参与 primary 唯一性约束；library/runtime 走 schema v2
///角色（role/parent 是 v2 特性，写出自动升版本）。
ca::Status registerRoleInputImpl(const fs::path&                    bundleDir,
                                 const std::string&                 containerPath,
                                 const std::string&                 fingerprint,
                                 mjbase::project::AnalysisInputRole role,
                                 const std::string&                 parent)
{
    const auto projectFile = bundleDir / "project.json";
    auto loaded = mjbase::project::readAnalysisProjectFile(utf8Ref(projectFile.string()));
    if (loaded.is_err()) return analysisProjectError(std::move(loaded).unwrap_err(), "read project entity");
    auto project = std::move(loaded).unwrap();

    const auto pathKey        = mjbase::project::copyLedgerText(containerPath);
    const auto fingerprintKey = mjbase::project::copyLedgerText(fingerprint);
    const auto parentKey      = mjbase::project::copyLedgerText(parent);
    if (const auto* existing = project.findInput(pathKey.ref())) {
        if (existing->fingerprint.ref().equals(fingerprintKey.ref()) &&
            existing->role == role &&
            existing->parent.ref().equals(parentKey.ref())) return ca::OkStatus();
        if (project.removeInput(pathKey.ref()) != 1)
            return ca::ErrStatus(ca::StatusCode::INTERNAL,
                                 "register container: stale input could not be removed");
    }
    AnalysisInput input;
    input.path        = pathKey.clone();
    input.fingerprint = fingerprintKey.clone();
    input.role        = role;
    input.parent      = parentKey.clone();
    if (!project.addInput(std::move(input)))
        return ca::ErrStatus(ca::StatusCode::INTERNAL,
                             "register container: input rejected");
    auto written = mjbase::project::writeAnalysisProjectFile(
        utf8Ref(projectFile.string()), project);
    if (written.is_err()) return analysisProjectError(std::move(written).unwrap_err(), "write project entity");
    return ca::OkStatus();
}

///角色化容器去登记（container_mount detach 挂点）：按路径移除；仅当登记角色
///与 expectedRole 一致时摘除（不误删其他角色的同路径条目），未登记为幂等
///no-op（登记面与会话内在册表短暂不一致时不阻塞 detach）。
ca::Status removeRoleInputImpl(const fs::path&                    bundleDir,
                               const std::string&                 containerPath,
                               mjbase::project::AnalysisInputRole expectedRole)
{
    const auto projectFile = bundleDir / "project.json";
    auto loaded = mjbase::project::readAnalysisProjectFile(utf8Ref(projectFile.string()));
    if (loaded.is_err()) return analysisProjectError(std::move(loaded).unwrap_err(), "read project entity");
    auto project = std::move(loaded).unwrap();
    const auto pathKey = mjbase::project::copyLedgerText(containerPath);
    const auto* existing = project.findInput(pathKey.ref());
    if (existing == nullptr) return ca::OkStatus();
    if (existing->role != expectedRole) return ca::OkStatus();
    if (project.removeInput(pathKey.ref()) != 1)
        return ca::ErrStatus(ca::StatusCode::INTERNAL,
                             "remove container: stale input could not be removed");
    auto written = mjbase::project::writeAnalysisProjectFile(
        utf8Ref(projectFile.string()), project);
    if (written.is_err()) return analysisProjectError(std::move(written).unwrap_err(), "write project entity");
    return ca::OkStatus();
}

}   //namespace

///@brief ProjectStoreError → ca::Status（声明见头；kind 决定码：锁冲突按占用
///        反馈面回 PERMISSION_DENIED，schema 违约回 INVALID_ARGUMENT，section
///        损坏回 DATA_LOSS，缺件回 NOT_FOUND，其余内部错误）。
ca::Status projectStoreStatus(mjbase::project::ProjectStoreError error, const char* operation)
{
    using mjbase::project::ProjectStoreErrorKind;
    ca::StatusCode code = ca::StatusCode::INTERNAL;
    switch (error.kind) {
    case ProjectStoreErrorKind::Open: code = ca::StatusCode::NOT_FOUND; break;
    case ProjectStoreErrorKind::Lock: code = ca::StatusCode::PERMISSION_DENIED; break;
    case ProjectStoreErrorKind::Schema: code = ca::StatusCode::INVALID_ARGUMENT; break;
    case ProjectStoreErrorKind::Section: code = ca::StatusCode::DATA_LOSS; break;
    case ProjectStoreErrorKind::Io: break;
    }
    return ca::ErrStatus(code,
                         std::string(operation) + ": " + error.message.to_std_string());
}

ProjectSession::~ProjectSession()
{
    //先析构 workspace：台账引擎随对象释放 store.lock（Windows 持锁句柄会阻止
    //目录删除），临时 bundle 的清理必须排在锁释放之后。
    workspace.reset();
    if (temporary && !projectDir.empty()) {
        std::error_code ec;
        fs::remove_all(projectDir, ec);
    }
}

ca::Status openProjectSession(const std::string&               jarPath,
                              const std::string&               projectDir,
                              std::shared_ptr<ProjectSession>& outSession,
                              bool&                            outCreated,
                              std::string&                     outFingerprint)
{
    outSession.reset();
    outCreated    = false;
    outFingerprint.clear();

    std::error_code ec;
    const fs::path jar = fs::absolute(fs::path(jarPath)).lexically_normal();
    if (!fs::exists(jar, ec) || ec)
        return ca::ErrStatus(ca::StatusCode::NOT_FOUND, "source not found: " + jar.string());

    //工程目录判别：空 = 临时工程（同样落完整 bundle 骨架）；显式目录必须是
    //完整 bundle（缺件提前报人读错误而不是让 bindLedgerStore 报引擎错误），
    //缺失 = 新建骨架。
    std::string dirText;
    bool        created = false;
    if (projectDir.empty()) {
        const auto status = makeTemporaryProjectDir(dirText);
        if (status.is_err()) return status;
        auto skeleton = mjbase::project::createBundleSkeleton(utf8Ref(dirText));
        if (skeleton.is_err())
            return projectStoreStatus(std::move(skeleton).unwrap_err(), "create project bundle");
        created = true;
    }
    else {
        dirText = fs::absolute(fs::path(projectDir)).lexically_normal().string();
        const auto kind = mjbase::project::classifyStorePath(utf8Ref(dirText));
        if (kind == mjbase::project::StorePathKind::BareFile ||
            kind == mjbase::project::StorePathKind::Other)
            return ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                 "project path is not a directory: " + dirText);
        if (kind == mjbase::project::StorePathKind::Directory) {
            auto manifest = mjbase::project::readStoreManifestFile(utf8Ref(dirText));
            if (manifest.is_err())
                return projectStoreStatus(std::move(manifest).unwrap_err(), "open project bundle");
        }
        else {
            auto skeleton = mjbase::project::createBundleSkeleton(utf8Ref(dirText));
            if (skeleton.is_err())
                return projectStoreStatus(std::move(skeleton).unwrap_err(), "create project bundle");
            created = true;
        }
    }

    std::string fingerprint;
    const auto fingerprintStatus = fileFingerprint(jar, fingerprint);
    if (fingerprintStatus.is_err()) return fingerprintStatus;
    const auto registered = registerPrimaryInput(dirText, jar.string(), fingerprint);
    if (registered.is_err()) return registered;

    auto session = std::make_shared<ProjectSession>();
    session->projectDir = dirText;
    session->temporary  = projectDir.empty();
    session->workspace  = std::make_unique<mjt::JarWorkspace>();
    const auto opened = session->workspace->openJar(jar.string());
    if (opened.is_err()) return shared::toCaStatus(opened.unwrap_err());
    //单写者锁与活动代装载在 bindLedgerStore 内完成；失败即锁被活进程持有或
    //台账损毁，会话不落半开状态。
    const auto bound = session->workspace->bindLedgerStore(dirText);
    if (bound.is_err()) return shared::toCaStatus(bound.unwrap_err());
    //分析缓存绑定同一 bundle（纯数据绑定零触盘；清单不可载 = 会话不落半开，
    //与台账同一 fail-closed 纪律）。缓存引擎不获取 store.lock——单写者锁已归
    //台账引擎，写路径前置条件（调用方持锁）由会话串行面保证。
    auto cache = mjbase::project::AnalysisCacheStore::open(utf8Ref(dirText));
    if (cache.is_err())
        return projectStoreStatus(std::move(cache).unwrap_err(), "open analysis cache");
    session->cacheStore =
        std::make_shared<mjbase::project::AnalysisCacheStore>(std::move(cache).unwrap());
    //角色化容器栈：primary>library>runtime 解析序；容器对象借用自
    //mountedContainers（所有权归会话）。
    session->containerStack = std::make_unique<mjbase::model::MountedContainerSet>();

    outSession     = std::move(session);
    outCreated     = created;
    outFingerprint = fingerprint;
    return ca::OkStatus();
}

ca::Result<PersistedLedgerSnapshot, mjbase::project::ProjectStoreError> readPersistedLedger(
    const std::string& bundleDir)
{
    using mjbase::project::kLedgerSectionPrefix;
    using mjbase::project::StoreSectionEntry;

    PersistedLedgerSnapshot snapshot;
    auto loaded = mjbase::project::readStoreManifestFile(utf8Ref(bundleDir));
    if (loaded.is_err()) return ca::Err(std::move(loaded).unwrap_err());
    snapshot.manifest = std::move(loaded).unwrap();

    //活动代 = 清单中 edit-ledger-<n> 的最大 n（引擎装载同口径）。
    const StoreSectionEntry* active = nullptr;
    ca::u32                  generation = 0;
    const std::string        prefix = kLedgerSectionPrefix;
    for (const auto& entry : snapshot.manifest.sections) {
        const auto name = entry.name.ref().to_std_string();
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
        const auto tail = name.substr(prefix.size());
        if (tail.empty() || tail.size() > 10) continue;
        ca::u32 value = 0;
        const auto* begin = tail.data();
        const auto* end   = begin + tail.size();
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc() || parsed.ptr != end) continue;
        if (active == nullptr || value > generation) {
            generation = value;
            active     = &entry;
        }
    }
    if (active == nullptr) return ca::Ok(std::move(snapshot));
    snapshot.generation = generation;

    auto bytes = mjbase::project::readSectionBytes(utf8Ref(bundleDir), *active);
    if (bytes.is_err()) return ca::Err(std::move(bytes).unwrap_err());
    auto rows = mjbase::project::parseLedgerRows(std::move(bytes).unwrap().ref());
    if (rows.is_err()) return ca::Err(std::move(rows).unwrap_err());
    snapshot.rows = std::move(rows).unwrap();
    mjbase::project::sortLedgerRows(snapshot.rows);
    return ca::Ok(std::move(snapshot));
}

std::string primaryFingerprint(const std::string& bundleDir)
{
    const auto projectFile = (fs::path(bundleDir) / "project.json").string();
    auto loaded = mjbase::project::readAnalysisProjectFile(utf8Ref(projectFile));
    if (loaded.is_err()) return {};
    //inputs() 返回容器内引用：必须先物化 project 再遍历，不能在临时对象上
    //链式取引用（range-for 只延长最外层临时，中间对象先析构成悬垂）。
    auto project = std::move(loaded).unwrap();
    for (const auto& input : project.inputs()) {
        if (input.role != mjbase::project::AnalysisInputRole::Primary) continue;
        return input.fingerprint.to_std_string();
    }
    return {};
}

ca::Status registerSupportInput(const std::string& bundleDir,
                                const std::string& containerPath,
                                const std::string& fingerprint)
{
    return registerRoleInputImpl(bundleDir, containerPath, fingerprint,
                                 mjbase::project::AnalysisInputRole::Support, {});
}

ca::Status removeSupportInput(const std::string& bundleDir, const std::string& containerPath)
{
    return removeRoleInputImpl(bundleDir, containerPath,
                               mjbase::project::AnalysisInputRole::Support);
}

ca::Status registerRoleInput(const std::string&                 bundleDir,
                             const std::string&                 containerPath,
                             const std::string&                 fingerprint,
                             mjbase::project::AnalysisInputRole role,
                             const std::string&                 parent)
{
    return registerRoleInputImpl(bundleDir, containerPath, fingerprint, role, parent);
}

ca::Status removeRoleInput(const std::string&                 bundleDir,
                           const std::string&                 containerPath,
                           mjbase::project::AnalysisInputRole expectedRole)
{
    return removeRoleInputImpl(bundleDir, containerPath, expectedRole);
}

ca::Status containerFingerprint(const std::string& containerPath, std::string& out)
{
    return fileFingerprint(fs::path(containerPath), out);
}

ca::Status flushAnalysisCache(ProjectSession& session)
{
    if (!session.cacheStore) return ca::OkStatus();
    std::lock_guard<std::mutex> lock(session.cachePending->mutex);
    auto flushed =
        session.cacheStore->flush(mjbase::project::rfc3339UtcNow());
    if (flushed.is_err())
        return projectStoreStatus(std::move(flushed).unwrap_err(), "flush analysis cache");
    //记录已发布进 section：pending 镜像清空，后续查询走 store 读路径。
    session.cachePending->sources.clear();
    return ca::OkStatus();
}

const ProjectSession::MountedContainerRecord* findMountedContainer(
    const ProjectSession& session, const std::string& id)
{
    for (const auto& record : session.mountedContainers) {
        if (record.id == id) return &record;
    }
    return nullptr;
}

std::map<std::string, std::string> mountedClassInventory(const ProjectSession&    session,
                                                         mjbase::model::ContainerRole role)
{
    std::map<std::string, std::string> inventory;
    if (!session.containerStack) return inventory;
    //直接遍历挂载记录（visitRole 的访问器首参是条目 URI，不带容器 id）。
    for (const auto& mounted : session.containerStack->containers()) {
        if (mounted.role != role) continue;
        const std::string containerId = mounted.id.to_std_string();
        mounted.container->visitResources(
            [&](std::string_view, const mjbase::model::IResource& resource) {
                const auto* classResource = ca::core::tag_cast::dyn_cast<
                    const mjbase::model::ClassResource>(&resource);
                if (classResource == nullptr) return;
                const auto& bytes = classResource->get();
                auto parsed = jbc::core::readClass(jbc::core::ByteView(bytes));
                if (parsed.is_err()) return;
                const auto name = std::string{std::move(parsed).unwrap().file->thisName()};
                if (!name.empty()) inventory.emplace(name, containerId);
            });
    }
    return inventory;
}

}   //namespace mjt::mcp
