#include "shared/jar_remapper.hpp"

#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "mbase/asm/remapper/resource_remapper.hpp"
#include "mbase/jar/jar_util.hpp"
#include "mbase/mapping/mapping_format_registry.hpp"
#include "mbase/model/resource_helper.hpp"
#include "shared/file_io.hpp"
#include "shared/i18n_errors.hpp"

namespace mjt {
namespace {

using namespace mjt::base;
using workspace::ImpactAnalysis;
using workspace::MutationRequest;
using workspace::MutationResult;
using workspace::IWorkspace;

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
            FieldRemapRule rule;
            rule.oldOwner = owner;
            rule.oldName  = field.oldName;
            rule.oldDesc  = field.desc;
            rule.newOwner = plan.mapInternalName(owner);
            rule.newName  = field.newName;
            rule.newDesc  = plan.mapFieldDescriptor(field.desc);
            plan.mapField(rule);
        }
    }

    for (const auto& [owner, methods] : mappings.getMethods()) {
        for (const auto& method : methods) {
            MethodRemapRule rule;
            rule.oldOwner = owner;
            rule.oldName  = method.oldName;
            rule.oldDesc  = method.desc;
            rule.newOwner = plan.mapInternalName(owner);
            rule.newName  = method.newName;
            rule.newDesc  = plan.mapMethodDescriptor(method.desc);
            plan.mapMethod(rule);
        }
    }
}

void addPackageClassMappings(const std::vector<std::string>& classNames,
                             const IntermediateMapping& mappings, MemberRemapPlan& plan)
{
    for (const auto& internalName : classNames) {
        const auto        mapped       = mappings.getMappedClassName(
            ca::str::Utf8StringRef::from_data(
                reinterpret_cast<const ca::u8*>(internalName.data()), internalName.size()));
        if (mapped.is_empty()) continue;

        if (!mapped.equals(ca::str::Utf8StringRef::from_cstr(internalName.c_str())))
            plan.mapClass(ca::str::Utf8StringRef::from_cstr(internalName.c_str()), mapped);
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

ca::Status buildPlan(const std::vector<std::string>& classNames, const std::string& format,
                     const std::string& mappingText, const std::string& policyName,
                     MemberRemapPlan& plan, RemapPolicy& policy)
{
    if (format.empty() || mappingText.empty()) {
        return shared::errStatus(ca::StatusCode::INVALID_ARGUMENT,
                                 "mjt.shared.err.ws_remap_missing");
    }
    auto registry = MappingFormatRegistry::withStandardFormats();
    const auto formatRef = ca::str::Utf8StringRef::from_cstr(format.c_str());
    if (!registry.hasFormat(formatRef)) {
        return shared::errStatus(ca::StatusCode::INVALID_ARGUMENT,
                                 "mjt.shared.err.unknown_mapping_format", {format});
    }
    if (!parsePolicy(policyName, policy)) {
        return shared::errStatus(ca::StatusCode::INVALID_ARGUMENT,
                                 "mjt.shared.err.unknown_remap_policy", {policyName});
    }
    try {
        const auto mappings = registry.parse(
            formatRef, ca::str::Utf8StringRef::from_data(
                           reinterpret_cast<const ca::u8*>(mappingText.data()), mappingText.size()));
        //package 规则先按当前工作区实际 class 展开，再构造成员规则；成员的新 owner 和
        //descriptor 才能基于完整 class plan 计算，保证声明和引用始终使用同一映射。
        addExplicitClassMappings(mappings, plan);
        addPackageClassMappings(classNames, mappings, plan);
        addMemberMappings(mappings, plan);
        return ca::OkStatus();
    }
    catch (const std::exception& exception) {
        return shared::errStatus(ca::StatusCode::INVALID_ARGUMENT,
                                 "mjt.shared.err.parse_mapping_failed", {exception.what()});
    }
}

std::vector<std::string> classNames(const ResourceContainer& container)
{
    std::vector<std::string> names;
    container.visitResources([&](const std::string& uri, const IResource& resource) {
        if (resource.getType() != ResourceType::Class) return;
        constexpr const char kSuffix[] = ".class";
        if (uri.size() <= sizeof(kSuffix) - 1 ||
            uri.compare(uri.size() - (sizeof(kSuffix) - 1), sizeof(kSuffix) - 1, kSuffix) != 0) {
            return;
        }
        names.push_back(uri.substr(0, uri.size() - (sizeof(kSuffix) - 1)));
    });
    return names;
}

std::vector<std::string> classNames(IWorkspace& workspace, ca::Status& status)
{
    std::vector<std::string> names;
    auto classes = workspace.listClasses();
    if (classes.is_err()) {
        status = classes.unwrap_err();
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
    if (!JarUtil::isJarFile(request.inputPath) || !JarUtil::isJarFile(request.outputPath)) {
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
        MemberRemapPlan   plan;
        ResourceContainer container;
        ResourceHelper::importFromJar(container, request.inputPath);
        auto status = buildPlan(classNames(container), request.format, mappingText, request.policy,
                                plan, policy);
        if (status.is_err()) return ca::Err(std::move(status));

        ca::str::Utf8StringArena arena;
        const bool               changed = applyResourceRemapPlan(container, arena, plan, policy);
        ResourceHelper::exportToJar(container, request.outputPath);
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

ca::Status buildWorkspaceRemapPlan(IWorkspace& workspace, const WorkspaceRemapRequest& request,
                                   MemberRemapPlan& plan, RemapPolicy& policy)
{
    ca::Status status = ca::OkStatus();
    auto names = classNames(workspace, status);
    if (status.is_err()) return status;
    return buildPlan(names, request.format, request.mappingText, request.policy, plan, policy);
}

ImpactAnalysis analyzeWorkspaceRemap(IWorkspace& workspace, const WorkspaceRemapRequest& request)
{
    ImpactAnalysis  result;
    MemberRemapPlan plan;
    RemapPolicy     policy;
    auto status = buildWorkspaceRemapPlan(workspace, request, plan, policy);
    if (status.is_err()) {
        result.status = std::move(status);
        return result;
    }
    return workspace.analyzeRemap(plan, policy);
}

MutationResult applyWorkspaceRemap(IWorkspace& workspace, const WorkspaceRemapRequest& request,
                                   const MutationRequest& mutation)
{
    MemberRemapPlan plan;
    RemapPolicy     policy;
    auto status = buildWorkspaceRemapPlan(workspace, request, plan, policy);
    if (status.is_err()) {
        MutationResult result;
        result.status     = std::move(status);
        result.revision   = workspace.revision();
        result.exportable = workspace.canExport();
        return result;
    }
    return workspace.applyRemap(plan, policy, mutation);
}

}   //namespace mjt
