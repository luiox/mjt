#include "shared/jar_remapper.hpp"

#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "mjbase/asm/remapper/resource_remapper.hpp"
#include "mjbase/jar/jar_util.hpp"
#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/mapping/mapping_format_registry.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "shared/file_io.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/status_convert.hpp"

namespace mjt {
namespace {

using mjbase::asm_remapper::MemberRemapPlan;
using mjbase::asm_remapper::RemapPolicy;
using mjbase::mapping::IntermediateMapping;

///i18n 文本 → WorkspaceErrorInfo（与 errStatus 同源的呈现面，错误通道换
///mjbase workspace 的值语义）。
mjbase::workspace::WorkspaceErrorInfo errInfo(mjbase::workspace::WorkspaceError code,
                                              const char*                                key,
                                              std::initializer_list<std::string_view> args = {})
{
    mjbase::workspace::WorkspaceErrorInfo info;
    info.code    = code;
    info.message = shared::trfStr(key, args);
    return info;
}

///mjbase mapping 解析错误 → 短文本（MappingError 无 to_string，此处仅三个值）。
const char* mappingErrorText(mjbase::mapping::MappingError error)
{
    switch (error) {
    case mjbase::mapping::MappingError::InvalidText: return "invalid mapping text";
    case mjbase::mapping::MappingError::UnknownFormat: return "unknown mapping format";
    case mjbase::mapping::MappingError::Io: return "mapping io failure";
    }
    return "mapping failure";
}

///mjbase 模型错误 → 短文本（ModelError 有 to_string，直接复用）。
std::string modelErrorText(mjbase::model::ModelError error)
{
    return mjbase::model::to_string(error);
}

void addExplicitClassMappings(const IntermediateMapping& mappings, MemberRemapPlan& plan)
{
    for (const auto& [oldName, mapping] : mappings.getClasses()) {
        (void)oldName;
        plan.mapClass(mapping.oldName, mapping.newName);
    }
}

void addMemberMappings(const IntermediateMapping& mappings, MemberRemapPlan& plan)
{
    for (const auto& [owner, fields] : mappings.getFields()) {
        for (const auto& field : fields) {
            //新 owner/desc 先经 plan 解析（package 规则已先行入 plan），保证
            //声明与引用始终使用同一映射；text 重载把六段文本 intern 进计划池。
            const std::string_view mappedOwner = plan.mapInternalName(owner);
            const std::string_view mappedDesc  = plan.mapFieldDescriptor(field.desc);
            plan.mapField(owner, field.oldName, field.desc, mappedOwner, field.newName,
                          mappedDesc);
        }
    }

    for (const auto& [owner, methods] : mappings.getMethods()) {
        for (const auto& method : methods) {
            const std::string_view mappedOwner = plan.mapInternalName(owner);
            const std::string_view mappedDesc  = plan.mapMethodDescriptor(method.desc);
            plan.mapMethod(owner, method.oldName, method.desc, mappedOwner, method.newName,
                           mappedDesc);
        }
    }
}

void addPackageClassMappings(const std::vector<std::string>& classNames,
                             const IntermediateMapping& mappings, MemberRemapPlan& plan)
{
    for (const auto& internalName : classNames) {
        const auto mapped = mappings.getMappedClassName(
            ca::str::Utf8StringRef::from_data(
                reinterpret_cast<const ca::u8*>(internalName.data()), internalName.size()));
        if (mapped.is_empty()) continue;

        if (!mapped.equals(ca::str::Utf8StringRef::from_cstr(internalName.c_str())))
            plan.mapClass(internalName, mapped);
    }
}

bool parsePolicy(const std::string& name, RemapPolicy& policy)
{
    if (name.empty() || name == "bytecode-only") {
        policy = RemapPolicy::bytecodeOnly();
        return true;
    }
    if (name == "reflection") {
        policy = RemapPolicy::bytecodeAndReflectionStrings();
        return true;
    }
    if (name == "metadata") {
        policy = RemapPolicy::bytecodeAndMetadata();
        return true;
    }
    if (name == "all") {
        policy = RemapPolicy::all();
        return true;
    }
    return false;
}

mjbase::workspace::WorkspaceErrorInfo buildPlan(const std::vector<std::string>& classNames,
                                                const std::string&              format,
                                                const std::string&              mappingText,
                                                const std::string&              policyName,
                                                MemberRemapPlan& plan, RemapPolicy& policy)
{
    if (format.empty() || mappingText.empty()) {
        return errInfo(mjbase::workspace::WorkspaceError::InvalidArgument,
                       "mjt.shared.err.ws_remap_missing");
    }
    auto registry = mjbase::mapping::MappingFormatRegistry::withStandardFormats();
    const auto formatRef = ca::str::Utf8StringRef::from_cstr(format.c_str());
    if (!registry.hasFormat(formatRef)) {
        return errInfo(mjbase::workspace::WorkspaceError::InvalidArgument,
                       "mjt.shared.err.unknown_mapping_format", {format});
    }
    if (!parsePolicy(policyName, policy)) {
        return errInfo(mjbase::workspace::WorkspaceError::InvalidArgument,
                       "mjt.shared.err.unknown_remap_policy", {policyName});
    }
    auto parsed = registry.parse(
        formatRef, ca::str::Utf8StringRef::from_data(
                       reinterpret_cast<const ca::u8*>(mappingText.data()), mappingText.size()));
    if (parsed.is_err()) {
        return errInfo(mjbase::workspace::WorkspaceError::InvalidArgument,
                       "mjt.shared.err.parse_mapping_failed",
                       {mappingErrorText(std::move(parsed).unwrap_err())});
    }
    //package 规则先按当前工作区实际 class 展开，再构造成员规则；成员的新 owner 和
    //descriptor 才能基于完整 class plan 计算，保证声明和引用始终使用同一映射。
    auto mappings = std::move(parsed).unwrap();
    addExplicitClassMappings(mappings, plan);
    addPackageClassMappings(classNames, mappings, plan);
    addMemberMappings(mappings, plan);
    return {};
}

std::vector<std::string> classNames(const mjbase::model::ResourceContainer& container)
{
    std::vector<std::string> names;
    container.visitResources([&](std::string_view uri, const mjbase::model::IResource& resource) {
        if (resource.getType() != mjbase::model::ResourceType::Class) return;
        constexpr std::string_view kSuffix = ".class";
        if (uri.size() <= kSuffix.size() || uri.substr(uri.size() - kSuffix.size()) != kSuffix) {
            return;
        }
        names.emplace_back(uri.substr(0, uri.size() - kSuffix.size()));
    });
    return names;
}

std::vector<std::string> classNames(const mjbase::workspace::IWorkspace&         workspace,
                                    mjbase::workspace::WorkspaceErrorInfo&       status)
{
    std::vector<std::string> names;
    auto classes = workspace.listClasses();
    if (classes.is_err()) {
        status = std::move(classes).unwrap_err();
        return names;
    }
    for (const auto& clazz : classes.unwrap()) names.push_back(clazz.internalName);
    return names;
}

}   //namespace

ca::StatusOr<bool> remapJar(const JarRemapRequest& request)
{
    if (request.inputPath.empty() || request.outputPath.empty() || request.mappingPath.empty() ||
        request.format.empty()) {
        return ca::Err(shared::errStatus(ca::StatusCode::INVALID_ARGUMENT,
                                         "mjt.shared.err.remap_missing_args"));
    }
    if (!mjbase::jar::JarUtil::isJarFile(ca::str::Utf8StringRef::from_cstr(
            request.inputPath.c_str())) ||
        !mjbase::jar::JarUtil::isJarFile(
            ca::str::Utf8StringRef::from_cstr(request.outputPath.c_str()))) {
        return ca::Err(
            shared::errStatus(ca::StatusCode::INVALID_ARGUMENT, "mjt.shared.err.remap_jar_ext"));
    }

    RemapPolicy policy;
    std::string mappingText;
    if (!readTextFile(request.mappingPath, mappingText)) {
        return ca::Err(shared::errStatus(ca::StatusCode::NOT_FOUND,
                                         "mjt.shared.err.read_mapping_failed",
                                         {request.mappingPath}));
    }

    try {
        MemberRemapPlan             plan;
        mjbase::model::ResourceContainer container;
        auto imported = mjbase::model::ResourceHelper::importFromJar(container, request.inputPath);
        if (imported.is_err()) {
            return ca::Err(shared::errStatus(ca::StatusCode::INTERNAL,
                                             "mjt.shared.err.remap_exception",
                                             {modelErrorText(std::move(imported).unwrap_err())}));
        }
        auto status =
            buildPlan(classNames(container), request.format, mappingText, request.policy, plan, policy);
        if (status.is_err()) return ca::Err(shared::toCaStatus(status));

        auto remapped = mjbase::asm_remapper::applyResourceRemapPlan(container, plan, policy);
        if (remapped.is_err()) {
            return ca::Err(shared::errStatus(
                ca::StatusCode::INTERNAL, "mjt.shared.err.remap_exception",
                {mjbase::asm_remapper::to_string(std::move(remapped).unwrap_err())}));
        }
        const bool changed = remapped.unwrap();
        auto exported = mjbase::model::ResourceHelper::exportToJar(container, request.outputPath);
        if (exported.is_err()) {
            return ca::Err(shared::errStatus(ca::StatusCode::INTERNAL,
                                             "mjt.shared.err.remap_exception",
                                             {modelErrorText(std::move(exported).unwrap_err())}));
        }
        return ca::Ok(changed);
    }
    catch (const std::exception& exception) {
        return ca::Err(shared::errStatus(ca::StatusCode::INTERNAL, "mjt.shared.err.remap_exception",
                                         {exception.what()}));
    }
    catch (...) {
        return ca::Err(shared::errStatus(ca::StatusCode::INTERNAL, "mjt.shared.err.remap_failed"));
    }
}

mjbase::workspace::WorkspaceErrorInfo buildWorkspaceRemapPlan(
    mjbase::workspace::IWorkspace& workspace, const WorkspaceRemapRequest& request,
    MemberRemapPlan& plan, RemapPolicy& policy)
{
    mjbase::workspace::WorkspaceErrorInfo status;
    auto names = classNames(workspace, status);
    if (status.is_err()) return status;
    return buildPlan(names, request.format, request.mappingText, request.policy, plan, policy);
}

mjbase::workspace::ImpactAnalysis analyzeWorkspaceRemap(
    mjbase::workspace::IWorkspace& workspace, const WorkspaceRemapRequest& request)
{
    mjbase::workspace::ImpactAnalysis result;
    MemberRemapPlan plan;
    RemapPolicy     policy;
    auto status = buildWorkspaceRemapPlan(workspace, request, plan, policy);
    if (status.is_err()) {
        result.status = std::move(status);
        return result;
    }
    return workspace.analyzeRemap(plan, policy);
}

mjbase::workspace::MutationResult applyWorkspaceRemap(
    mjbase::workspace::IWorkspace& workspace, const WorkspaceRemapRequest& request,
    const mjbase::workspace::MutationRequest& mutation)
{
    MemberRemapPlan plan;
    RemapPolicy     policy;
    auto status = buildWorkspaceRemapPlan(workspace, request, plan, policy);
    if (status.is_err()) {
        mjbase::workspace::MutationResult result;
        result.status     = std::move(status);
        result.revision   = workspace.revision();
        result.exportable = workspace.canExport();
        return result;
    }
    return workspace.applyRemap(plan, policy, mutation);
}

}   //namespace mjt
