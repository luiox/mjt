#include "analysis_edit.hpp"

#include <string>
#include <unordered_map>
#include <utility>

#include "jbc/core/reader.hpp"

#include "mjbase/analysis/semantic_fp.hpp"
#include "mjbase/jasm/classfile_utf8.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/analysis_db.hpp"

namespace mjt::workspace {
namespace {

/// @brief 库查找/落库用的规范形 UTF-8 名 → 借用视图（字节边界直通）。
ca::str::Utf8StringRef utf8RefOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// @brief class DOM 成员名/描述符（常量池 modified UTF-8）→ 规范形文本。
///
/// 库内身份（analysis_ingest 登记的 name/descriptor）与 mutation 选择器都是
/// 规范形；DOM 侧一律先过同一转换再比对，混淆语料的代理对名字不失配。
std::string canonicalText(const jbc::core::ClassFile& cf, ca::u32 cpIndex)
{
    return mjbase::jasm::classFileUtf8ToString(cf.utf8(cpIndex));
}

/// @brief 按 (name, descriptor) 在 DOM 方法表里精确定位成员；未命中 nullptr。
jbc::core::MemberInfo* findMethodDom(jbc::core::ClassFile&   cf,
                                     const std::string&      name,
                                     const std::string&      descriptor)
{
    for (jbc::core::MemberInfo& member : cf.methods) {
        if (canonicalText(cf, member.nameCp) == name &&
            canonicalText(cf, member.descCp) == descriptor)
            return &member;
    }
    return nullptr;
}

/// @brief 解析 class 资源字节为 DOM；解析失败返回空（调用方计数跳过）。
std::unique_ptr<jbc::core::ClassFile> parseClassResource(const model::ResourceContainer& container,
                                                         const ca::str::Utf8StringRef&   uri)
{
    const auto* resource =
        ca::core::tag_cast::dyn_cast<const model::ClassResource>(container.getResource(
            std::string_view(reinterpret_cast<const char*>(uri.data()), uri.byte_length())));
    if (resource == nullptr) return nullptr;
    auto read = jbc::core::readClass(jbc::core::ByteView(resource->get()));
    if (read.is_err()) return nullptr;
    return std::move(read).unwrap().file;
}

/// @brief 差异行是否指向 class 资源（manifest 等非 class 差异直通不计数）。
bool isClassResource(const model::ResourceContainer& container, const ca::str::Utf8StringRef& uri)
{
    return ca::core::tag_cast::dyn_cast<const model::ClassResource>(container.getResource(
               std::string_view(reinterpret_cast<const char*>(uri.data()), uri.byte_length()))) !=
           nullptr;
}

/// @brief 单方法面：聚焦成员指纹落库（recordMethodVersion 口径）。
bool recordFocusedMethod(mjbase::project::AnalysisDb&      db,
                         const EditedMethodFocus&          focus,
                         jbc::core::ClassFile&             cf,
                         const std::string&                owner,
                         MethodVersionRecordStats&         stats)
{
    auto* member = findMethodDom(cf, focus.memberName, focus.memberDescriptor);
    if (member == nullptr) {
        // 移除型 mutation 的目标已不在编辑后字节里：无指纹可算，如实跳过。
        ++stats.skipped;
        return true;
    }
    auto fp = mjbase::analysis::semanticFp(cf, *member);
    if (fp.is_err()) {
        ++stats.skipped;
        return true;
    }

    auto foundClass = db.findClass(utf8RefOf(owner));
    if (foundClass.is_err()) {
        stats.failed = true;
        return false;
    }
    const auto classInfo = std::move(foundClass).unwrap();
    if (!classInfo.has_value()) {
        ++stats.skipped;
        return true;
    }

    auto found = db.findMember(mjbase::project::DbMemberKind::Method, utf8RefOf(owner),
                               utf8RefOf(focus.memberName), utf8RefOf(focus.memberDescriptor));
    if (found.is_err()) {
        stats.failed = true;
        return false;
    }
    const auto memberInfo = std::move(found).unwrap();
    if (!memberInfo.has_value()) {
        ++stats.skipped;
        return true;
    }

    auto current = db.currentMethodFingerprint(memberInfo->memberId);
    if (current.is_err()) {
        stats.failed = true;
        return false;
    }
    const auto& existing = std::move(current).unwrap();
    if (existing.has_value() && *existing == fp.unwrap()) {
        // 语义没变（字节变了指纹不变的归一化场景）：幂等跳过，不落伪版本。
        ++stats.unchanged;
        return true;
    }

    auto recorded =
        db.recordMethodVersion(classInfo->classId, memberInfo->memberId, fp.unwrap());
    if (recorded.is_err()) {
        stats.failed = true;
        return false;
    }
    ++stats.recorded;
    return true;
}

}   // namespace

MethodVersionRecordStats recordMethodVersionsAfterEdit(
    mjbase::project::AnalysisDb&            db,
    const model::ResourceContainer&         container,
    const std::vector<ca::str::Utf8String>& classUris,
    const EditedMethodFocus*                focus)
{
    MethodVersionRecordStats stats;
    for (const ca::str::Utf8String& uri : classUris) {
        if (!isClassResource(container, uri.ref())) continue;
        auto cf = parseClassResource(container, uri.ref());
        if (cf == nullptr) {
            // class 字节解析失败：无 DOM 可算指纹，计一条跳过。
            ++stats.skipped;
            continue;
        }
        const std::string owner = mjbase::jasm::classFileUtf8ToString(cf->thisName());
        if (owner.empty()) {
            stats.skipped += cf->methods.size();
            continue;
        }

        // 单方法面：聚焦成员命中本类时只记它（其余成员语义必然未动）。
        if (focus != nullptr && focus->isMethod && focus->ownerInternalName == owner) {
            if (!recordFocusedMethod(db, *focus, *cf, owner, stats)) return stats;
            continue;
        }

        // 批量面：整类重算，语义未动成员由 mjbase 幂等跳过。
        auto foundClass = db.findClass(utf8RefOf(owner));
        if (foundClass.is_err()) {
            stats.failed = true;
            return stats;
        }
        const auto classInfo = std::move(foundClass).unwrap();
        if (!classInfo.has_value()) {
            stats.skipped += cf->methods.size();
            continue;
        }

        auto members = db.listMembers(utf8RefOf(owner));
        if (members.is_err()) {
            stats.failed = true;
            return stats;
        }
        std::unordered_map<std::string, ca::i64> methodIdByName;
        for (const mjbase::project::DbMemberInfo& row : std::move(members).unwrap()) {
            if (row.kind != mjbase::project::DbMemberKind::Method) continue;
            methodIdByName.emplace(row.name.to_std_string() + " " + row.descriptor.to_std_string(),
                                   row.memberId);
        }

        std::vector<mjbase::project::DbMethodVersionInput> inputs;
        inputs.reserve(cf->methods.size());
        for (jbc::core::MemberInfo& member : cf->methods) {
            const std::string name = canonicalText(*cf, member.nameCp);
            const std::string desc = canonicalText(*cf, member.descCp);
            const auto        id   = methodIdByName.find(name + " " + desc);
            if (id == methodIdByName.end()) {
                ++stats.skipped;   // 未登记方法（新增成员等）：身份登记归导入桥。
                continue;
            }
            auto fp = mjbase::analysis::semanticFp(*cf, member);
            if (fp.is_err()) {
                ++stats.skipped;
                continue;
            }
            mjbase::project::DbMethodVersionInput input;
            input.classId    = classInfo->classId;
            input.memberId   = id->second;
            input.fingerprint = std::move(fp).unwrap();
            inputs.push_back(std::move(input));
        }
        if (inputs.empty()) continue;
        auto recorded = db.recordMethodVersions(std::move(inputs));
        if (recorded.is_err()) {
            stats.failed = true;
            return stats;
        }
        const auto counts = std::move(recorded).unwrap();
        stats.recorded += counts.recorded;
        stats.unchanged += counts.unchanged;
    }
    return stats;
}

}   // namespace mjt::workspace
