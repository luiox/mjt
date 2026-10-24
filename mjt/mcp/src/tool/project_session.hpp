#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "libca/core/result.hpp"
#include "libca/core/status.hpp"

#include "mjbase/model/container_stack.hpp"
#include "mjbase/project/analysis_cache.hpp"
#include "mjbase/project/analysis_project.hpp"
#include "mjbase/project/edit_ledger.hpp"
#include "mjbase/project/project_store.hpp"

#include "shared/jar_workspace.hpp"

namespace mjt::mcp {

///@brief 一个已打开的分析会话：workspace 实例与其绑定的工程 bundle。
///
///工程接线（RFC 0001 P1）：每个会话都落在工程 bundle 上——显式工程目录或
///系统临时目录下的临时工程，不再有"无工程内存态"。bundle 的单写者锁
///（store.lock）由 workspace 内嵌的台账引擎在 bindLedgerStore 时获取并持有
///到 workspace 析构；primary 容器登记与 sha256 指纹写 project.json；台账
///落盘走 flushLedger（project_close 与导出路径的挂点）。
///
///分析缓存（RFC 0001 P3）：cacheStore 绑定同一 bundle 的缓存 section 面。
///缓存引擎不获取 store.lock（单写者锁已归台账引擎），put/flush 的内存
///pending 面由 cacheMutex 串行化（async 反编译 worker 线程与执行体线程共
///享）；flush 挂点与 flushLedger 同点（project_close / project_export 前）。
struct ProjectSession final
{
    ProjectSession() = default;
    ~ProjectSession();

    ProjectSession(const ProjectSession&)            = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;

    std::unique_ptr<mjt::JarWorkspace> workspace;
    ///bundle 目录（绝对路径）；store.lock/project.json/store.json 都在其下。
    std::string projectDir;
    ///临时工程（调用方未给工程目录）：析构时随会话整体删除，对齐旧"无工程
    ///内存态"随进程蒸发的语义。
    bool temporary = false;

    ///分析缓存存取（与台账同 bundle；open 失败即会话不落半开状态）。两者都
    ///用 shared_ptr：async 反编译 worker 线程持有副本，会话析构后迟到的
    ///put 只是随 store 蒸发的 pending（缓存可重算，无损正确性）。
    std::shared_ptr<mjbase::project::AnalysisCacheStore> cacheStore;
    ///pending 镜像（slot 名+类名+digest → 源码）：put 驻内存期间查询可见
    ///（AnalysisCacheStore.lookupDecompile 只读已 flush 的 section，不读
    ///pending）；flush 成功后清空——记录已在盘上，store 读路径接管。互斥量
    ///一并串行化 store.pending 写面与命中计数（async worker 与执行体共享）。
    struct CachePendingMirror final
    {
        std::mutex                         mutex;
        std::map<std::string, std::string> sources;
    };
    std::shared_ptr<CachePendingMirror> cachePending = std::make_shared<CachePendingMirror>();
    ///会话内命中计数（project_info 缓存健康的运行时面；cachePending.mutex 保护）。
    ca::u64 cacheHits = 0;
    ca::u64 cacheMisses = 0;

    ///挂载的 library/runtime 容器（RFC 0001 P5 角色化容器栈；容器对象所有权
    ///归会话，containerStack 只借用）。support 遗留角色仍走 workspace 在册
    ///表，不入本栈。
    struct MountedContainerRecord final
    {
        std::string id;            ///< 工程内唯一（登记 path 的规范化绝对路径）
        mjbase::model::ContainerRole role = mjbase::model::ContainerRole::Library;
        std::string path;          ///< 登记 path（= id）
        std::string fingerprint;   ///< 登记指纹（"sha256:"+64hex，文件字节口径）
        std::string supportId;     ///< workspace 侧在册 id（xref 跨容器参与面）
        std::shared_ptr<mjbase::model::ResourceContainer> container;  ///< 挂载内容
    };
    std::vector<MountedContainerRecord> mountedContainers;
    ///角色化容器栈（primary>library>runtime 解析序、shadowed 记录、角色遍历）。
    std::unique_ptr<mjbase::model::MountedContainerSet> containerStack;
};

///@brief 打开工程会话：创建/复用 bundle、登记 primary 容器、绑定台账引擎。
///
///@param jarPath 主 JAR 路径（必须存在）。
///@param projectDir 工程目录；空 = 系统临时目录下新建临时工程。已存在的
///        目录必须是完整 bundle；已登记其他 primary 容器的工程拒绝换绑。
///@param outSession 成功时持有会话。
///@param outCreated bundle 是否本次新建。
///@param outFingerprint primary 容器的登记指纹（"sha256:"+64hex，文件字节口径）。
ca::Status openProjectSession(const std::string&        jarPath,
                              const std::string&        projectDir,
                              std::shared_ptr<ProjectSession>& outSession,
                              bool&                     outCreated,
                              std::string&              outFingerprint);

///@brief 已落盘台账的活动链快照（工程总览与台账列览共用的只读面）。
///
///查询型工具先经 flushLedger 把 pending 落盘（崩溃安全、幂等、不动 revision），
///再从 bundle 读活动代台账——磁盘上的活动链因此与内存头指针一致。行序为
///装载定序：锚行（若有）在首、事务按 txn 升序、书签按名序。
struct PersistedLedgerSnapshot final
{
    mjbase::project::StoreManifest          manifest;   ///< 当前清单（含 ledger_config）
    std::vector<mjbase::project::LedgerRow> rows;       ///< 活动链行
    ca::u32                                 generation = 0; ///< 活动代（0 = 尚无落盘台账）
};

///@brief 读 bundle 的活动代台账；无台账 section 时返回空链快照（generation 0）。
ca::Result<PersistedLedgerSnapshot, mjbase::project::ProjectStoreError> readPersistedLedger(
    const std::string& bundleDir);

///@brief 读 project.json 中 primary 输入的登记指纹；无登记或读不了 = 空串
///        （仅诊断呈现用，不参与数据面）。
std::string primaryFingerprint(const std::string& bundleDir);

///@brief support 容器登记（container_mount mount 挂点）：同 path 幂等，指纹
///        变化即刷新；写入 project.json 的 inputs（role=support）。
ca::Status registerSupportInput(const std::string& bundleDir,
                                const std::string& containerPath,
                                const std::string& fingerprint);

///@brief support 容器去登记（container_mount detach 挂点）：按路径移除，
///        未登记时幂等 no-op。
ca::Status removeSupportInput(const std::string& bundleDir, const std::string& containerPath);

///@brief 角色化容器登记（container_mount library/runtime 挂点）：同 path 幂等
///        按角色刷新；parent 非空时登记挂载父容器（schema v2 挂载树）。
///        support 遗留登记由 registerSupportInput 承接，不混用本入口。
ca::Status registerRoleInput(const std::string&                   bundleDir,
                             const std::string&                   containerPath,
                             const std::string&                   fingerprint,
                             mjbase::project::AnalysisInputRole   role,
                             const std::string&                   parent);

///@brief 角色化容器去登记（container_mount detach 挂点）：按路径移除；仅当
///        登记角色与 expectedRole 一致时摘除（support 遗留条目不受影响），
///        未登记时幂等 no-op。
ca::Status removeRoleInput(const std::string&                 bundleDir,
                           const std::string&                 containerPath,
                           mjbase::project::AnalysisInputRole expectedRole);

///@brief 文件字节的 sha256 登记指纹（"sha256:"+64hex；容器挂载登记口径）。
ca::Status containerFingerprint(const std::string& containerPath, std::string& out);

///@brief 缓存合并 flush（project_close / project_export 挂点；与 flushLedger
///        同点）：把 pending 条目级记录按槽并写落盘并退役同族陈旧槽。无
///        pending = no-op；串行化经会话 cacheMutex。
ca::Status flushAnalysisCache(ProjectSession& session);

///@brief 挂载记录按 id 查找；未命中 nullptr。
const ProjectSession::MountedContainerRecord* findMountedContainer(
    const ProjectSession& session, const std::string& id);

///@brief 挂载栈指定角色的类清点：类内部名 → 容器 id（visitRole 扫描 + 类名
///        探针；畸形类跳过）。角色过滤查询的站点归属面；容器内容不可变，
///        每次调用重建（挂载集合量级小）。
std::map<std::string, std::string> mountedClassInventory(
    const ProjectSession& session, mjbase::model::ContainerRole role);

///@brief ProjectStoreError → ca::Status（kind 决定码；供 MCP 工具面统一转换）。
ca::Status projectStoreStatus(mjbase::project::ProjectStoreError error, const char* operation);

}   //namespace mjt::mcp
