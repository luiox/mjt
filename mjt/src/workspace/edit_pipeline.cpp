#include <utility>

#include "libca/core/tag_cast.hpp"

#include "mjbase/jar/manifest.hpp"
#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/asm/remapper/resource_remapper.hpp"
#include "mjbase/model/resource_helper.hpp"

#include "class_mutation_service.hpp"
#include "edit_command.hpp"

#include "analysis_ops.hpp"
#include "edit_support.hpp"
#include "jar_ops.hpp"
#include "workspace.hpp"

namespace mjt::workspace {

/// @file edit_pipeline.cpp
/// @brief 编辑单入口实现：EditCommand 变体 × MutationRequest 一条管道。
///
/// EditPipeline 经 workspace.hpp 的 friend 声明取得 WorkspaceBase 私有事务面
/// （ensureLoaded/accepts/commit/cloneWorking/validateCandidate…）；handler 表按
/// EditCommand::Op 的 alternative 序注册——新增编辑形态 = edit_command.hpp 加
/// 载荷 + 本结构加一个静态 handler + 表里加一行，公共类头签名不动。

/// @brief 编辑管道内核：每变体一个静态 handler（workspace.hpp friend）。
struct EditPipeline
{
    using Handler = MutationResult (*)(WorkspaceBase&, const EditCommand&,
                                       const MutationRequest&);

    /// @brief adapter 候选字节（旧 validateClassBytes/applyClassBytes 合流）。
    static MutationResult classBytes(WorkspaceBase& ws, const EditCommand& cmd,
                                     const MutationRequest& request)
    {
        const auto& op     = std::get<EditCommand::ClassBytes>(cmd.op);
        const auto  uri    = op.uri.to_std_string();
        auto        loaded = ws.ensureLoaded("apply class bytes");
        if (loaded.is_err()) return ws.stateFailure(std::move(loaded));
        if (!ws.accepts(request)) return ws.revisionConflict();
        if (op.bytes.empty()) {
            return ws.stateFailure(
                error(WorkspaceError::InvalidArgument, "apply class bytes",
                      "candidate class is empty"));
        }
        try {
            auto candidate = ws.cloneWorking();
            const auto current = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
                candidate->getResource(uri));
            if (current == nullptr) {
                return ws.stateFailure(
                    error(WorkspaceError::NotFound, "apply class bytes",
                          "class resource not found: " + uri));
            }
            candidate->replaceResource(uri,
                                       std::make_unique<model::ClassResource>(uri, op.bytes));

            if (request.dryRun) {
                MutationResult result;
                result.revision   = ws.revision();
                result.exportable = ws.canExport();
                // 门禁作用域 = 本次替换的类。
                result.status     = ws.validateCandidate(*candidate, result.diagnostics,
                                                         request.validationMode, {uri});
                if (result.status.is_err()) return result;
                result.byteDiffs = diffContainers(*ws.workingContainer(), *candidate);
                result.affectedClasses =
                    utf8List(changedClassNames(*candidate, result.byteDiffs));
                result.affectedReferences =
                    collectImpacts(*candidate, result.byteDiffs, result.affectedClasses);
                return result;
            }
            std::vector<jasm::Diagnostic> diagnostics;
            // 门禁作用域 = 本次替换的类。
            auto status =
                ws.validateCandidate(*candidate, diagnostics, request.validationMode, {uri});
            if (status.is_err()) {
                MutationResult result;
                result.status      = std::move(status);
                result.revision    = ws.revision();
                result.exportable  = ws.canExport();
                result.diagnostics = std::move(diagnostics);
                return result;
            }
            return ws.commit(std::move(candidate), std::move(diagnostics), request);
        }
        catch (const std::exception& exception) {
            return ws.stateFailure(
                error(WorkspaceError::Internal, "apply class bytes", exception.what()));
        }
    }

    /// @brief 原始字节替换（尺寸门/CAFEBABE 门/内名一致性门；dry 内折）。
    static MutationResult replaceClassBytes(WorkspaceBase& ws, const EditCommand& cmd,
                                            const MutationRequest& request)
    {
        const auto& op       = std::get<EditCommand::ReplaceClassBytes>(cmd.op);
        const auto  selector = op.selector.to_std_string();
        auto        loaded   = ws.ensureLoaded("replace class bytes");
        if (loaded.is_err()) return ws.stateFailure(std::move(loaded));
        if (!ws.accepts(request)) return ws.revisionConflict();
        if (op.bytes.empty() || op.bytes.size() > kMaximumClassReplacementSize)
            return ws.stateFailure(error(WorkspaceError::InvalidArgument, "replace class bytes",
                                         "class size must be in 1..16777216 bytes"));
        if (op.bytes.size() < 4 || op.bytes[0] != 0xca || op.bytes[1] != 0xfe ||
            op.bytes[2] != 0xba || op.bytes[3] != 0xbe)
            return ws.stateFailure(error(WorkspaceError::InvalidArgument, "replace class bytes",
                                         "candidate does not start with CAFEBABE"));

        auto resolved = resolveClassUri(*ws.workingContainer(), selector);
        if (resolved.is_err()) return ws.stateFailure(std::move(resolved).unwrap_err());
        const auto uri                  = std::move(resolved).unwrap();
        const auto expectedInternalName = model::internalNameFromClassUri(uri);
        if (expectedInternalName.empty())
            return ws.stateFailure(
                error(WorkspaceError::InvalidArgument, "replace class bytes",
                      "target URI is not a canonical class or multi-release class URI"));
        const auto* current = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
            ws.workingContainer()->getResource(uri));
        if (current == nullptr)
            return ws.stateFailure(error(WorkspaceError::NotFound, "replace class bytes", uri));

        try {
            const auto actualInternalName = internalNameOfBytes(op.bytes);
            if (actualInternalName != expectedInternalName)
                return ws.stateFailure(
                    error(WorkspaceError::InvalidArgument, "replace class bytes",
                          "candidate internal name does not match target URI"));

            auto candidate = ws.cloneWorking();
            candidate->replaceResource(
                uri, std::make_unique<model::ClassResource>(locationText(*current), op.bytes));
            std::vector<jasm::Diagnostic> diagnostics;
            // 门禁作用域 = 本次替换的类。
            auto status = ws.validateCandidate(*candidate, diagnostics,
                                               request.validationMode, {uri});
            if (status.is_err()) {
                MutationResult result = ws.stateFailure(std::move(status));
                result.diagnostics    = std::move(diagnostics);
                return result;
            }
            if (!request.dryRun)
                return ws.commit(std::move(candidate), std::move(diagnostics), request);

            MutationResult result;
            result.status      = WorkspaceErrorInfo{};
            result.revision    = ws.revision();
            result.exportable  = ws.canExport();
            result.diagnostics = std::move(diagnostics);
            result.byteDiffs   = diffContainers(*ws.workingContainer(), *candidate);
            result.affectedClasses =
                utf8List(changedClassNames(*candidate, result.byteDiffs));
            result.affectedReferences =
                collectImpacts(*candidate, result.byteDiffs, result.affectedClasses);
            return result;
        }
        catch (const std::exception& exception) {
            return ws.stateFailure(error(WorkspaceError::InvalidArgument, "replace class bytes",
                                         exception.what()));
        }
    }

    /// @brief 完整 class JASM 汇编（旧 validateJasm/applyJasm 合流）。
    static MutationResult jasm(WorkspaceBase& ws, const EditCommand& cmd,
                               const MutationRequest& request)
    {
        const auto& op  = std::get<EditCommand::Jasm>(cmd.op);
        const auto  uri = op.uri.to_std_string();
        auto loaded     = ws.ensureLoaded("apply JASM");
        if (loaded.is_err()) return ws.stateFailure(std::move(loaded));
        if (!ws.accepts(request)) return ws.revisionConflict();
        // 选择器与结构化补丁同口径：裸内部名按容器字节内容解析到规范 URI。
        auto resolvedUri = resolveClassUri(*ws.workingContainer(), uri);
        if (resolvedUri.is_err()) return ws.stateFailure(std::move(resolvedUri).unwrap_err());
        try {
            auto                 candidate = ws.cloneWorking();
            ClassMutationService service(*candidate);
            const std::string    resolved  = std::move(resolvedUri).unwrap();
            // 诊断定位名缺省 = URI + ".jasm"（旧工具层口径）。
            const ca::str::Utf16String sourceName =
                op.sourceName.is_empty() ? utf16(resolved + ".jasm") : op.sourceName.clone();
            auto assembled = service.applyJasm(ca::str::Utf8StringRef::from_string_view(resolved),
                                               sourceName.ref(), op.source.ref(),
                                               request.validationMode, op.frameMode);
            if (!assembled.isOk()) {
                MutationResult result;
                result.status = error(WorkspaceError::InvalidArgument,
                                      request.dryRun ? "validate JASM" : "apply JASM",
                                      "candidate class did not pass validation");
                result.revision    = ws.revision();
                result.exportable  = ws.canExport();
                result.diagnostics = std::move(assembled.diagnostics);
                return result;
            }
            if (request.dryRun) {
                MutationResult result;
                result.revision    = ws.revision();
                result.exportable  = ws.canExport();
                result.diagnostics = std::move(assembled.diagnostics);
                result.status      = WorkspaceErrorInfo{};
                result.byteDiffs   = diffContainers(*ws.workingContainer(), *candidate);
                result.affectedClasses =
                    utf8List(changedClassNames(*candidate, result.byteDiffs));
                result.affectedReferences =
                    collectImpacts(*candidate, result.byteDiffs, result.affectedClasses);
                return result;
            }
            return ws.commit(std::move(candidate), std::move(assembled.diagnostics), request);
        }
        catch (const std::exception& exception) {
            return ws.stateFailure(error(WorkspaceError::Internal, "apply JASM", exception.what()));
        }
    }

    /// @brief 结构化 class 修改（旧 validateClassMutation/applyClassMutation 合流）。
    static MutationResult classMutation(WorkspaceBase& ws, const EditCommand& cmd,
                                        const MutationRequest& request)
    {
        const auto& op = std::get<EditCommand::ClassMutation>(cmd.op);
        auto loaded    = ws.ensureLoaded("apply class mutation");
        if (loaded.is_err()) return ws.stateFailure(std::move(loaded));
        // 选择器契约对齐 replaceClassBytes：精确资源 URI 直接命中，内部名按字节
        // 内容解析到规范 URI。
        auto resolvedUri = resolveClassUri(*ws.workingContainer(),
                                              op.mutation.classUri.to_std_string());
        if (resolvedUri.is_err()) return ws.stateFailure(std::move(resolvedUri).unwrap_err());
        ClassMutation resolvedMutation = op.mutation.clone();
        resolvedMutation.classUri      = utf8(std::move(resolvedUri).unwrap());
        if (!ws.accepts(request)) return ws.revisionConflict();
        try {
            auto                 candidate = ws.cloneWorking();
            ClassMutationService service(*candidate);
            auto outcome = service.apply(resolvedMutation, request.validationMode);
            if (outcome.status.is_err()) {
                MutationResult result;
                result.status      = std::move(outcome.status);
                result.revision    = ws.revision();
                result.exportable  = ws.canExport();
                result.diagnostics = std::move(outcome.diagnostics);
                return result;
            }
            if (request.dryRun) {
                MutationResult result;
                result.revision    = ws.revision();
                result.exportable  = ws.canExport();
                result.diagnostics = std::move(outcome.diagnostics);
                result.status      = WorkspaceErrorInfo{};
                result.byteDiffs   = diffContainers(*ws.workingContainer(), *candidate);
                result.affectedClasses =
                    utf8List(changedClassNames(*candidate, result.byteDiffs));
                result.affectedReferences =
                    collectImpacts(*candidate, result.byteDiffs, result.affectedClasses);
                return result;
            }
            // 单方法寻址 kind 的指纹落库走聚焦单方法面（recordMethodVersion）；
            // 其余 kind 聚焦为空，commit 锚点按整类批量面兜底。
            EditedMethodFocus focus = focusedMethodOfEdit(resolvedMutation);
            return ws.commit(std::move(candidate), std::move(outcome.diagnostics), request,
                             focus.ownerInternalName.empty() ? nullptr : &focus);
        }
        catch (const std::exception& exception) {
            return ws.stateFailure(
                error(WorkspaceError::Internal, "apply class mutation", exception.what()));
        }
    }

    /// @brief 显式重映射计划（旧 applyRemap；dry 经 analyzeRemap 预览）。
    static MutationResult remapPlan(WorkspaceBase& ws, const EditCommand& cmd,
                                    const MutationRequest& request)
    {
        const auto& op = std::get<EditCommand::RemapPlan>(cmd.op);
        auto loaded    = ws.ensureLoaded("apply remap");
        if (loaded.is_err()) return ws.stateFailure(std::move(loaded));
        if (!ws.accepts(request)) return ws.revisionConflict();
        if (request.dryRun) {
            auto analysis = ws.analyzeRemap(*op.plan, op.policy);
            MutationResult result;
            result.status             = std::move(analysis.status);
            result.revision           = ws.revision();
            result.exportable         = analysis.exportable;
            result.diagnostics        = std::move(analysis.diagnostics);
            result.byteDiffs          = std::move(analysis.byteDiffs);
            result.affectedReferences = std::move(analysis.afterReferences);
            result.affectedClasses =
                utf8List(changedClassNames(*ws.workingContainer(), result.byteDiffs));
            return result;
        }
        try {
            auto candidate = ws.cloneWorking();
            auto remapped  = asm_remapper::applyResourceRemapPlan(*candidate, *op.plan, op.policy);
            if (remapped.is_err()) {
                return ws.stateFailure(
                    error(WorkspaceError::Internal, "apply remap",
                          std::string("remap failed: ") +
                              asm_remapper::to_string(std::move(remapped).unwrap_err())));
            }
            std::vector<jasm::Diagnostic> diagnostics;
            // 门禁作用域 = remap 改动的类。
            const auto byteDiffs = diffContainers(*ws.workingContainer(), *candidate);
            auto status = ws.validateCandidate(*candidate, diagnostics, request.validationMode,
                                               changedUris(byteDiffs));
            if (status.is_err()) {
                MutationResult result;
                result.status      = std::move(status);
                result.revision    = ws.revision();
                result.exportable  = ws.canExport();
                result.diagnostics = std::move(diagnostics);
                return result;
            }
            // 身份注记：真改名（rename/mapping 经此漏斗）的类/成员规格随事务
            // 落账并迁移分析库（shadow 落分析库——覆盖层按稳定 ID 键控）。
            return ws.commit(std::move(candidate), std::move(diagnostics), request, nullptr,
                             ws.identitySpecsOfRemap(*op.plan));
        }
        catch (const std::exception& exception) {
            return ws.stateFailure(error(WorkspaceError::Internal, "apply remap", exception.what()));
        }
    }

    /// @brief 重命名（旧 applyRename：override 风险门 → local 走结构化 /
    ///        其余走 remap 漏斗）。
    static MutationResult rename(WorkspaceBase& ws, const EditCommand& cmd,
                                 const MutationRequest& request)
    {
        const auto& op = std::get<EditCommand::Rename>(cmd.op);
        auto preview   = ws.analyzeRename(op.request);
        if (preview.status.is_err()) return ws.stateFailure(std::move(preview.status));
        if (!preview.readyToApply) {
            return ws.stateFailure(
                error(WorkspaceError::FailedPrecondition, "apply rename",
                      "rename requires override review: " +
                          (preview.impact.risks.empty()
                               ? std::string("unknown risk")
                               : preview.impact.risks.front().to_std_string())));
        }
        if (op.request.kind == RenameKind::Local) {
            ClassMutation mutation;
            mutation.classUri             = utf8(op.request.owner.ref());
            mutation.kind                 = ClassMutationKind::RenameLocal;
            mutation.member = {utf8(op.request.methodName.ref()), utf8(op.request.descriptor.ref())};
            mutation.localName            = utf8(op.request.name.ref());
            mutation.replacementLocalName = utf8(op.request.newName.ref());
            mutation.localIndex           = op.request.localIndex;
            EditCommand local;
            local.op = EditCommand::ClassMutation{std::move(mutation)};
            return classMutation(ws, local, request);
        }
        asm_remapper::MemberRemapPlan plan;
        std::vector<RenameMapping>      mappings;
        std::vector<MethodOverrideInfo> overrides;
        std::vector<std::string>        risks;
        auto status = ws.buildRenamePlan(op.request, plan, mappings, overrides, risks);
        if (status.is_err()) return ws.stateFailure(std::move(status));
        EditCommand remap;
        // RemapPolicy 载荷禁拷贝（Utf8String 面）——借用须经显式 clone()。
        remap.op = EditCommand::RemapPlan{&plan, op.request.policy.clone()};
        return remapPlan(ws, remap, request);
    }

    /// @brief pass/pipeline（旧 validatePipeline/applyPipeline 合流）。
    static MutationResult pipeline(WorkspaceBase& ws, const EditCommand& cmd,
                                   const MutationRequest& request)
    {
        const auto& op = std::get<EditCommand::Pipeline>(cmd.op);
        auto loaded    = ws.ensureLoaded(request.dryRun ? "validate pipeline" : "apply pipeline");
        if (loaded.is_err()) return ws.stateFailure(std::move(loaded));
        if (!ws.accepts(request)) return ws.revisionConflict();
        try {
            auto candidate = ws.cloneWorking();
            auto status    = ws.runPipeline(*candidate, op.request);
            if (status.is_err()) return ws.stateFailure(std::move(status));

            if (request.dryRun) {
                MutationResult result;
                result.revision   = ws.revision();
                result.exportable = ws.canExport();
                // 门禁作用域 = pipeline 改动的类；diff 只算一次，scope 与报告共用。
                auto byteDiffs = diffContainers(*ws.workingContainer(), *candidate);
                result.status   = ws.validateCandidate(*candidate, result.diagnostics,
                                                       request.validationMode,
                                                       changedUris(byteDiffs));
                if (result.status.is_err()) return result;
                result.byteDiffs       = std::move(byteDiffs);
                result.affectedClasses = utf8List(
                    changedClassNames(*candidate, result.byteDiffs));
                result.affectedReferences =
                    collectImpacts(*candidate, result.byteDiffs, result.affectedClasses);
                return result;
            }

            std::vector<jasm::Diagnostic> diagnostics;
            // 门禁作用域 = pipeline 改动的类。
            const auto byteDiffs = diffContainers(*ws.workingContainer(), *candidate);
            status = ws.validateCandidate(*candidate, diagnostics, request.validationMode,
                                          changedUris(byteDiffs));
            if (status.is_err()) {
                MutationResult result;
                result.status      = std::move(status);
                result.revision    = ws.revision();
                result.exportable  = ws.canExport();
                result.diagnostics = std::move(diagnostics);
                return result;
            }
            return ws.commit(std::move(candidate), std::move(diagnostics), request);
        }
        catch (const std::exception& exception) {
            return ws.stateFailure(
                error(WorkspaceError::Internal, "apply pipeline", exception.what()));
        }
    }

    /// @brief jar 修复事务（旧 analyzeJarRepair/applyJarRepair 合流）。
    ///
    /// 报告面（issues/classCount）是 jar_ops 扫描自由函数（CLI 直读）；
    /// 管道只承接事务：候选 → 删选中条目 → 门禁（作用域 = 修复改动面）→
    /// dryRun 预览 / commit 落账。
    static MutationResult jarRepair(WorkspaceBase& ws, const EditCommand& cmd,
                                    const MutationRequest& request)
    {
        const auto& op = std::get<EditCommand::JarRepair>(cmd.op);
        auto loaded    = ws.ensureLoaded(request.dryRun ? "analyze JAR repair"
                                                       : "apply JAR repair");
        if (loaded.is_err()) return ws.stateFailure(std::move(loaded));
        if (!ws.accepts(request)) return ws.revisionConflict();
        try {
            auto                candidate = ws.cloneWorking();
            ca::usize           classCount = 0;
            const auto          issues = scanJarRepairIssues(*candidate, op.request, classCount);
            if (!hasSelectedRepairIssues(issues)) {
                MutationResult result;
                result.revision   = ws.revision();
                result.exportable = ws.canExport();
                result.status     = WorkspaceErrorInfo{};
                return result;
            }
            removeSelectedRepairIssues(*candidate, issues);

            // 门禁作用域 = 修复改动面：删除型修复的改动条目已不在候选中，
            // 其余条目未动不参与门禁（基线类编辑前就存在）。
            std::vector<jasm::Diagnostic> diagnostics;
            auto byteDiffs = diffContainers(*ws.workingContainer(), *candidate);
            auto status = ws.validateCandidate(*candidate, diagnostics,
                                               request.dryRun
                                                   ? jasm::ValidationMode::Relaxed
                                                   : request.validationMode,
                                               changedUris(byteDiffs));
            if (status.is_err()) {
                MutationResult result;
                result.status      = std::move(status);
                result.revision    = ws.revision();
                result.exportable  = ws.canExport();
                result.diagnostics = std::move(diagnostics);
                return result;
            }
            if (request.dryRun) {
                MutationResult result;
                result.revision    = ws.revision();
                result.exportable  = ws.canExport();
                result.diagnostics = std::move(diagnostics);
                result.status      = WorkspaceErrorInfo{};
                result.byteDiffs   = std::move(byteDiffs);
                result.affectedClasses =
                    utf8List(changedClassNames(*candidate, result.byteDiffs));
                result.affectedReferences =
                    collectImpacts(*candidate, result.byteDiffs, result.affectedClasses);
                return result;
            }
            return ws.commit(std::move(candidate), std::move(diagnostics), request);
        }
        catch (const std::exception& exception) {
            return ws.stateFailure(
                error(WorkspaceError::Internal, "apply JAR repair", exception.what()));
        }
    }

    /// @brief 泛条目字节写（upsert）：存在保持原形态置字节（Manifest 形按
    ///        文本重析，解析失败 → DataLoss）；不存在按内容判形。无门禁——
    ///        class 形编辑的门禁契约归 classBytes，本变体是字节面。
    static MutationResult resourceBytes(WorkspaceBase& ws, const EditCommand& cmd,
                                        const MutationRequest& request)
    {
        const auto& op  = std::get<EditCommand::ResourceBytes>(cmd.op);
        const auto  uri = op.uri.to_std_string();
        auto loaded = ws.ensureLoaded("apply resource bytes");
        if (loaded.is_err()) return ws.stateFailure(std::move(loaded));
        if (!ws.accepts(request)) return ws.revisionConflict();
        if (uri.empty()) {
            return ws.stateFailure(
                error(WorkspaceError::InvalidArgument, "apply resource bytes", "uri is empty"));
        }
        try {
            auto candidate = ws.cloneWorking();
            if (const auto* existing = candidate->getResource(uri)) {
                if (ca::core::tag_cast::isa<model::ClassResource>(existing)) {
                    candidate->replaceResource(
                        uri, std::make_unique<model::ClassResource>(uri, op.bytes));
                }
                else if (ca::core::tag_cast::isa<model::ManifestResource>(existing)) {
                    // fromBytes 静态直返（畸形文本抛异常，落入下方 catch 统一
                    // DataLoss）。
                    candidate->replaceResource(
                        uri, std::make_unique<model::ManifestResource>(
                                 mjbase::jar::Manifest::fromBytes(op.bytes)));
                }
                else {
                    candidate->replaceResource(
                        uri, std::make_unique<model::UnknownResource>(uri, op.bytes));
                }
            }
            else if (!internalNameOfBytes(op.bytes).empty()) {
                // 新增判形只按骨架解析（可解析出内名 → 类形），不看 URI 扩展
                // 名——SDK add_entry 契约：任意 URI（含非 .class 名）带类字节
                // 都按类条目登记。
                candidate->addResource(uri,
                                       std::make_unique<model::ClassResource>(uri, op.bytes));
            }
            else {
                candidate->addResource(
                    uri, std::make_unique<model::UnknownResource>(uri, op.bytes));
            }
            return ws.commit(std::move(candidate), {}, request);
        }
        catch (const std::exception& exception) {
            return ws.stateFailure(
                error(WorkspaceError::Internal, "apply resource bytes", exception.what()));
        }
    }

    /// @brief 条目删除：未登记 → NotFound；无门禁（不改余下条目字节）。
    static MutationResult removeResource(WorkspaceBase& ws, const EditCommand& cmd,
                                         const MutationRequest& request)
    {
        const auto& op  = std::get<EditCommand::RemoveResource>(cmd.op);
        const auto  uri = op.uri.to_std_string();
        auto loaded = ws.ensureLoaded("remove resource");
        if (loaded.is_err()) return ws.stateFailure(std::move(loaded));
        if (!ws.accepts(request)) return ws.revisionConflict();
        if (uri.empty()) {
            return ws.stateFailure(
                error(WorkspaceError::InvalidArgument, "remove resource", "uri is empty"));
        }
        try {
            auto candidate = ws.cloneWorking();
            if (candidate->getResource(uri) == nullptr) {
                return ws.stateFailure(
                    error(WorkspaceError::NotFound, "remove resource", uri));
            }
            candidate->getResources().erase(uri);
            return ws.commit(std::move(candidate), {}, request);
        }
        catch (const std::exception& exception) {
            return ws.stateFailure(
                error(WorkspaceError::Internal, "remove resource", exception.what()));
        }
    }

    /// @brief 条目 URI 改名搬移：资源按新 URI 重建（字节原样；任意资源形态）。
    ///        旧 URI 未登记 → NotFound；新 URI 已占用 → AlreadyExists。
    ///        无门禁（字节零变化；容器指纹随 URI 集变化）。
    static MutationResult renameResource(WorkspaceBase& ws, const EditCommand& cmd,
                                         const MutationRequest& request)
    {
        const auto& op     = std::get<EditCommand::RenameResource>(cmd.op);
        const auto  oldUri = op.oldUri.to_std_string();
        const auto  newUri = op.newUri.to_std_string();
        auto loaded = ws.ensureLoaded("apply resource rename");
        if (loaded.is_err()) return ws.stateFailure(std::move(loaded));
        if (!ws.accepts(request)) return ws.revisionConflict();
        if (oldUri.empty() || newUri.empty()) {
            return ws.stateFailure(error(WorkspaceError::InvalidArgument,
                                         "apply resource rename", "uri is empty"));
        }
        try {
            auto candidate = ws.cloneWorking();
            auto& storage  = candidate->getResources();
            if (storage.count(newUri) != 0) {
                return ws.stateFailure(error(WorkspaceError::AlreadyExists,
                                             "apply resource rename",
                                             "target uri already exists: " + newUri));
            }
            const auto it = storage.find(oldUri);
            if (it == storage.end()) {
                return ws.stateFailure(error(WorkspaceError::NotFound,
                                             "apply resource rename",
                                             "resource not found: " + oldUri));
            }
            // 资源对象必须按新 URI 重建而不能只搬 map 键：IResource 自带
            // location 是 jar 导出面（JarFileExporter）的条目名来源，仅换键
            // 会产出 entry↔location 失配的坏罐。
            auto moved = std::move(it->second);
            storage.erase(it);
            if (const auto* cr =
                    ca::core::tag_cast::dyn_cast<const model::ClassResource>(moved.get())) {
                candidate->addResource(newUri,
                                       std::make_unique<model::ClassResource>(newUri, cr->get()));
            }
            else if (const auto* mr =
                         ca::core::tag_cast::dyn_cast<const model::ManifestResource>(moved.get())) {
                candidate->addResource(newUri, std::make_unique<model::ManifestResource>(mr->get()));
            }
            else if (const auto* ur =
                         ca::core::tag_cast::dyn_cast<const model::UnknownResource>(moved.get())) {
                candidate->addResource(newUri,
                                       std::make_unique<model::UnknownResource>(newUri, ur->get()));
            }
            else {
                // 未知新形态：按原对象搬移（location 保真优先于重建假设）。
                candidate->addResource(newUri, std::move(moved));
            }
            return ws.commit(std::move(candidate), {}, request);
        }
        catch (const std::exception& exception) {
            return ws.stateFailure(
                error(WorkspaceError::Internal, "apply resource rename", exception.what()));
        }
    }

    /// @brief kind→handler 注册表：alternative 序 = EditCommand::Op 声明序。
    ///        新增编辑形态在此加一行（static_assert 钉住表长与变体数一致）。
    static constexpr Handler kHandlers[] = {
        &EditPipeline::classBytes, &EditPipeline::replaceClassBytes, &EditPipeline::jasm,
        &EditPipeline::classMutation, &EditPipeline::rename, &EditPipeline::remapPlan,
        &EditPipeline::pipeline, &EditPipeline::jarRepair, &EditPipeline::resourceBytes,
        &EditPipeline::removeResource, &EditPipeline::renameResource,
    };
    static_assert(std::variant_size_v<EditCommand::Op> == std::size(kHandlers),
                  "EditCommand variant count must match the handler table");

    static MutationResult run(WorkspaceBase& ws, const EditCommand& cmd,
                              const MutationRequest& request)
    {
        return kHandlers[cmd.op.index()](ws, cmd, request);
    }
};

MutationResult applyEdit(WorkspaceBase&         workspace,
                         const EditCommand&     command,
                         const MutationRequest& request)
{
    return EditPipeline::run(workspace, command, request);
}

WorkspaceResult<EditCommand::Jasm> spliceMethodJasm(WorkspaceBase&          workspace,
                                                    ca::str::Utf8StringRef owner,
                                                    ca::str::Utf8StringRef name,
                                                    ca::str::Utf8StringRef descriptor,
                                                    ca::str::Utf8StringRef newMethodSource,
                                                    ca::str::Utf8StringRef sourceName)
{
    // 1. 取单方法规范 Jasm（与 class Jasm 内方法段逐字节一致）。
    auto method = getMethodJasm(*workspace.workingContainer(), owner, name,
                                        descriptor);
    if (!method.isOk()) {
        return ca::core::Err(error(
            WorkspaceError::Internal, "splice method jasm",
            "method Jasm generation failed with " + std::to_string(method.diagnostics.size()) +
                " diagnostic(s)"));
    }
    // 2. 取整 class Jasm 作 base，把旧方法文本精确替换为新方法文本。
    auto classRow = classInfo(*workspace.workingContainer(), owner,
                              workspace.lvtJarFallbackFlag());
    if (classRow.is_err()) return ca::core::Err(std::move(classRow).unwrap_err());
    const std::string classUri = std::move(classRow).unwrap().resourceUri.to_std_string();
    auto jasm = getJasm(*workspace.workingContainer(), utf8Ref(classUri));
    if (!jasm.isOk()) {
        return ca::core::Err(error(
            WorkspaceError::Internal, "splice method jasm",
            "class Jasm generation failed with " + std::to_string(jasm.diagnostics.size()) +
                " diagnostic(s)"));
    }
    const std::string oldMethodSource = method.value->source.to_utf8_string().to_std_string();
    const std::string classSource     = jasm.value->source.to_utf8_string().to_std_string();
    const auto        first           = classSource.find(oldMethodSource);
    if (first == std::string::npos) {
        return ca::core::Err(error(WorkspaceError::Internal, "splice method jasm",
                                   "method source not found in class Jasm"));
    }
    const auto second = classSource.find(oldMethodSource, first + 1);
    if (second != std::string::npos) {
        return ca::core::Err(error(WorkspaceError::Internal, "splice method jasm",
                                   "method source is ambiguous in class Jasm"));
    }
    std::string newClassSource = classSource;
    newClassSource.replace(
        first, oldMethodSource.size(),
        std::string_view(reinterpret_cast<const char*>(newMethodSource.data()),
                         newMethodSource.byte_length()));
    EditCommand::Jasm payload;
    payload.uri = utf8(classUri);
    if (sourceName.is_empty()) {
        payload.sourceName =
            utf16(owner.to_std_string() + "." + name.to_std_string() + ".jasm");
    }
    else {
        payload.sourceName = ca::str::Utf16String::from_utf8_string(sourceName);
    }
    payload.source = utf16(newClassSource);
    return ca::core::Ok(std::move(payload));
}

}   // namespace mjt::workspace
