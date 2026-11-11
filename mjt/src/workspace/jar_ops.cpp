#include "jar_ops.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "libca/core/tag_cast.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"

#include "edit_support.hpp"

namespace mjt::workspace {

/// @file jar_ops.cpp
/// @brief jar 专属面自由函数实现（自 workspace.cpp 匿名命名空间上移）。

bool hasClassExtension(const std::string& uri)
{
    constexpr char kExtension[] = ".class";
    if (uri.size() < sizeof(kExtension) - 1) return false;
    const auto offset = uri.size() - (sizeof(kExtension) - 1);
    for (ca::usize index = 0; index < sizeof(kExtension) - 1; ++index) {
        const auto character = static_cast<unsigned char>(uri[offset + index]);
        if (std::tolower(character) != kExtension[index]) return false;
    }
    return true;
}

WorkspaceResult<std::vector<ManifestAttribute>> manifestAttributes(
    const mjbase::model::ResourceContainer& container)
{
    const model::ManifestResource* manifest = nullptr;
    container.visitResources([&](std::string_view, const mjbase::model::IResource& resource) {
        if (manifest == nullptr)
            manifest = ca::core::tag_cast::dyn_cast<const model::ManifestResource>(&resource);
    });
    if (manifest == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "read manifest", "MANIFEST.MF"));
    std::vector<ManifestAttribute> result;
    result.reserve(manifest->get().getMainAttributes().size());
    for (const auto& [key, value] : manifest->get().getMainAttributes()) {
        result.push_back({key.clone(), value.clone()});
    }
    std::sort(result.begin(), result.end(),
              [](const ManifestAttribute& left, const ManifestAttribute& right) {
                  return left.key < right.key;
              });
    return ca::core::Ok(std::move(result));
}

std::vector<JarRepairIssue> scanJarRepairIssues(const mjbase::model::ResourceContainer& container,
                                                const JarRepairRequest& request,
                                                ca::usize&              classCount)
{
    jasm::JvmJasmService        validator;
    std::vector<JarRepairIssue> issues;
    container.visitResources([&](std::string_view uriView, const model::IResource& resource) {
        const std::string uri{uriView};
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource != nullptr) {
            ++classCount;
            const auto validated = validator.validate(classResource->get());
            if (!validated.isOk()) {
                issues.push_back({utf8(uri),
                                  utf8("invalid_class"),
                                  utf8("class bytes did not pass Jasm validation"),
                                  classResource->get().size(),
                                  request.removeInvalidClassEntries});
            }
        }
        else if (hasClassExtension(uri)) {
            auto bytes = bytesOf(resource);
            issues.push_back({utf8(uri),
                              utf8("invalid_class_entry"),
                              utf8("a .class entry was not recognized as valid class bytes"),
                              bytes.is_ok() ? bytes.unwrap().size() : 0,
                              request.removeInvalidClassEntries});
        }
        if (uri.find("corrupt_output_stream") != std::string::npos) {
            auto bytes = bytesOf(resource);
            issues.push_back({utf8(uri),
                              utf8("corrupt_output_marker"),
                              utf8("resource name matches a known corrupt output marker"),
                              bytes.is_ok() ? bytes.unwrap().size() : 0,
                              request.removeCorruptOutputMarkers});
        }
    });
    std::sort(issues.begin(), issues.end(),
              [](const JarRepairIssue& left, const JarRepairIssue& right) {
                  if (left.uri != right.uri) return left.uri < right.uri;
                  return left.kind < right.kind;
              });
    return issues;
}

bool hasSelectedRepairIssues(const std::vector<JarRepairIssue>& issues)
{
    return std::any_of(issues.begin(), issues.end(), [](const JarRepairIssue& issue) {
        return issue.selectedForRemoval;
    });
}

void removeSelectedRepairIssues(mjbase::model::ResourceContainer&       candidate,
                                const std::vector<JarRepairIssue>& issues)
{
    // 容器以 string_view 键视图指向资源自持文本：先拷贝待删键再删除。
    auto&                    resources = candidate.getResources();
    std::vector<std::string> doomed;
    for (const auto& issue : issues) {
        if (issue.selectedForRemoval) doomed.push_back(issue.uri.to_std_string());
    }
    for (const std::string& uri : doomed) resources.erase(std::string_view(uri));
}

}   // namespace mjt::workspace
