#include "edit_support.hpp"

#include <algorithm>
#include <set>
#include <unordered_set>
#include <utility>

#include "libca/core/tag_cast.hpp"

#include "mjbase/jasm/classfile_utf8.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_helper.hpp"

#include "reference_scan.hpp"

namespace mjt::workspace {

WorkspaceErrorInfo error(WorkspaceError code, const std::string& operation,
                         const std::string& detail)
{
    return WorkspaceErrorInfo{code, operation + ": " + detail};
}

ca::str::Utf8String utf8(std::string_view value)
{
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

ca::str::Utf8StringRef utf8Ref(const std::string& value) noexcept
{
    return ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(utf8Ref(value));
}

std::vector<ca::str::Utf8String> utf8List(const std::vector<std::string>& values)
{
    std::vector<ca::str::Utf8String> result;
    result.reserve(values.size());
    for (const std::string& value : values) result.push_back(utf8(value));
    return result;
}

std::string locationText(const mjbase::model::IResource& resource)
{
    const auto location = resource.getLocation();
    return std::string(reinterpret_cast<const char*>(location.data()), location.byte_length());
}

WorkspaceResult<std::vector<ca::u8>> bytesOf(const mjbase::model::IResource& resource)
{
    if (const auto* classResource =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(&resource))
        return ca::core::Ok(classResource->get());
    if (const auto* manifestResource =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ManifestResource>(&resource))
        return ca::core::Ok(manifestResource->get().toBytes());
    if (const auto* unknownResource =
            ca::core::tag_cast::dyn_cast<const mjbase::model::UnknownResource>(&resource))
        return ca::core::Ok(unknownResource->get());
    return ca::core::Err(error(WorkspaceError::Unimplemented, "read resource",
                               "unsupported resource type at " + locationText(resource)));
}

std::vector<ByteDiff> diffContainers(const mjbase::model::ResourceContainer& before,
                                     const mjbase::model::ResourceContainer& after)
{
    std::set<std::string> uris;
    before.visitResources(
        [&](std::string_view uri, const mjbase::model::IResource&) {
            uris.insert(std::string(uri));
        });
    after.visitResources(
        [&](std::string_view uri, const mjbase::model::IResource&) {
            uris.insert(std::string(uri));
        });

    std::vector<ByteDiff> diffs;
    for (const auto& uri : uris) {
        const mjbase::model::IResource* beforeResource = before.getResource(uri);
        const mjbase::model::IResource* afterResource  = after.getResource(uri);
        std::vector<ca::u8>             beforeBytes;
        std::vector<ca::u8>             afterBytes;
        if (beforeResource != nullptr) {
            auto bytes = bytesOf(*beforeResource);
            if (bytes.is_err()) continue;
            beforeBytes = std::move(bytes).unwrap();
        }
        if (afterResource != nullptr) {
            auto bytes = bytesOf(*afterResource);
            if (bytes.is_err()) continue;
            afterBytes = std::move(bytes).unwrap();
        }
        if (beforeBytes == afterBytes) continue;

        // 单一公共前后缀收缩：第一个差异字节到最后一个差异字节（照旧口径）。
        ca::usize start = 0;
        while (start < beforeBytes.size() && start < afterBytes.size() &&
               beforeBytes[start] == afterBytes[start]) {
            ++start;
        }
        ca::usize beforeEnd = beforeBytes.size();
        ca::usize afterEnd  = afterBytes.size();
        while (beforeEnd > start && afterEnd > start &&
               beforeBytes[beforeEnd - 1] == afterBytes[afterEnd - 1]) {
            --beforeEnd;
            --afterEnd;
        }
        ByteDiff diff;
        diff.uri        = utf8(uri);
        diff.beforeSize = beforeBytes.size();
        diff.afterSize  = afterBytes.size();
        diff.ranges.push_back({start, beforeEnd - start, afterEnd - start});
        diffs.push_back(std::move(diff));
    }
    return diffs;
}

std::vector<std::string> changedUris(const std::vector<ByteDiff>& diffs)
{
    std::vector<std::string> uris;
    uris.reserve(diffs.size());
    for (const auto& diff : diffs) uris.push_back(diff.uri.to_std_string());
    return uris;
}

std::string internalNameOfBytes(const std::vector<ca::u8>& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) return {};
    auto parsed = std::move(read).unwrap();
    return jasm::classFileUtf8ToString(parsed.file->thisName());
}

std::vector<std::string> changedClassNames(const mjbase::model::ResourceContainer& container,
                                           const std::vector<ByteDiff>&            diffs)
{
    std::vector<std::string> result;
    for (const auto& diff : diffs) {
        const auto* resource = ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(
            container.getResource(diff.uri.ref()));
        if (resource == nullptr) continue;
        const auto name = internalNameOfBytes(resource->get());
        if (!name.empty()) result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

EditedMethodFocus focusedMethodOfEdit(const ClassMutation& mutation)
{
    using K          = ClassMutationKind;
    const bool methodKind =
        mutation.kind == K::SetMethodAccess || mutation.kind == K::ReplaceMethodBody ||
        mutation.kind == K::InsertInstruction || mutation.kind == K::ReplaceInstruction ||
        mutation.kind == K::RemoveInstruction || mutation.kind == K::ReplaceTryCatchBlocks ||
        mutation.kind == K::RenameLocal || mutation.kind == K::AddMethod ||
        mutation.kind == K::RemoveMethod;
    const bool annotationMethod =
        (mutation.kind == K::AddMarkerAnnotation || mutation.kind == K::RemoveAnnotation) &&
        mutation.annotationTarget == AnnotationTarget::Method;
    if (!methodKind && !annotationMethod) return {};
    const std::string owner =
        mjbase::model::internalNameFromClassUri(mutation.classUri.to_std_string());
    if (owner.empty() || mutation.member.name.to_std_string().empty()) return {};
    EditedMethodFocus focus;
    focus.ownerInternalName = owner;
    focus.memberName        = mutation.member.name.to_std_string();
    focus.memberDescriptor  = mutation.member.descriptor.to_std_string();
    focus.isMethod          = true;
    return focus;
}

namespace {

void appendUniqueImpact(std::vector<ReferenceImpact>&    impacts,
                        std::unordered_set<std::string>& seen, const std::string& kind,
                        const std::string& target)
{
    const std::string key = kind + "\n" + target;
    if (!seen.insert(key).second) return;
    impacts.push_back({utf8(kind), utf8(target)});
}

}   // namespace

std::vector<ReferenceImpact> collectImpacts(
    const mjbase::model::ResourceContainer&      container,
    const std::vector<ByteDiff>&                 diffs,
    const std::vector<ca::str::Utf8String>&      classes)
{
    try {
        mjbase::model::ResourceContainer scope;
        std::unordered_set<std::string>  scopedUris;
        for (const auto& diff : diffs) {
            const auto uri = diff.uri.to_std_string();
            if (!scopedUris.insert(uri).second) continue;
            const auto* classResource = ca::core::tag_cast::dyn_cast<
                const mjbase::model::ClassResource>(container.getResource(uri));
            if (classResource == nullptr) continue;   // 非 class 条目（MANIFEST 等）不进扫描
            scope.addResource(uri,
                              std::make_unique<mjbase::model::ClassResource>(
                                  locationText(*classResource), classResource->get()));
        }
        ReferenceScan                   index;
        index.build(scope);
        std::vector<ReferenceImpact>    impacts;
        std::unordered_set<std::string> seen;
        for (const auto& owner : classes) {
            for (const auto& reference : index.methodReferencesFromClass(owner.ref())) {
                appendUniqueImpact(impacts, seen, "method",
                                   reference.target.owner.to_std_string() + "." +
                                       reference.target.name.to_std_string() +
                                       reference.target.descriptor.to_std_string());
            }
            for (const auto& reference : index.fieldReferencesFromClass(owner.ref())) {
                appendUniqueImpact(impacts, seen, "field",
                                   reference.target.owner.to_std_string() + "." +
                                       reference.target.name.to_std_string() +
                                       reference.target.descriptor.to_std_string());
            }
            for (const auto& reference : index.typeReferencesFromClass(owner.ref())) {
                appendUniqueImpact(impacts, seen, "type", reference.targetType.to_std_string());
            }
        }
        std::sort(impacts.begin(), impacts.end(),
                  [](const ReferenceImpact& left, const ReferenceImpact& right) {
                      if (left.kind != right.kind) return left.kind < right.kind;
                      return left.target < right.target;
                  });
        return impacts;
    }
    catch (const std::exception&) {
        return {};
    }
}

}   // namespace mjt::workspace
