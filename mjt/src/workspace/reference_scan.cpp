#include "reference_scan.hpp"

#include <algorithm>
#include <unordered_set>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/analysis/cp_refs.hpp"
#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/class_file_sanitize.hpp"
#include "classfile_support.hpp"

namespace mjt::workspace {

namespace {

using jbc::core::Op;

/// @brief 底层文本（CP/会话 string_view）→ Utf8String：jbc 面文本按字节原文
///        直通，不做二次校验（与 analysis_ingest 的 utf8() 同口径）。
ca::str::Utf8String utf8(std::string_view value)
{
    return ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(value.data()),
                                                    value.size());
}

/// @brief invoke 家族（invokestatic/special/virtual/interface）；indy 由语义面单独分流。
bool isInvokeOp(Op op)
{
    return op == Op::INVOKEVIRTUAL || op == Op::INVOKESPECIAL || op == Op::INVOKESTATIC ||
           op == Op::INVOKEINTERFACE;
}

/// @brief 字段访问家族（getstatic/getfield/putstatic/putfield）。
bool isFieldAccessOp(Op op)
{
    return op == Op::GETSTATIC || op == Op::GETFIELD || op == Op::PUTSTATIC || op == Op::PUTFIELD;
}

/// @brief 显式类型指令（new/anewarray/checkcast/instanceof/multianewarray）。
bool isTypeOp(Op op)
{
    return op == Op::NEW || op == Op::ANEWARRAY || op == Op::CHECKCAST || op == Op::INSTANCEOF ||
           op == Op::MULTIANEWARRAY;
}

}   // namespace

void ReferenceScan::build(const model::ResourceContainer& container)
{
    buildFiltered(container, nullptr);
}

ReferenceScan ReferenceScan::fromParts(std::vector<MethodReference> methods,
                                       std::vector<FieldReference>  fields,
                                       std::vector<TypeReference>   types)
{
    ReferenceScan scan;
    scan.methods_ = std::move(methods);
    scan.fields_  = std::move(fields);
    scan.types_   = std::move(types);
    return scan;
}

void ReferenceScan::buildFor(const model::ResourceContainer&         container,
                             const std::vector<ca::str::Utf8String>& uris)
{
    std::unordered_set<std::string_view> wanted;
    wanted.reserve(uris.size());
    for (const ca::str::Utf8String& uri : uris) {
        wanted.emplace(reinterpret_cast<const char*>(uri.data()), uri.byte_length());
    }
    buildFiltered(container, &wanted);
}

void ReferenceScan::buildFiltered(const model::ResourceContainer&                container,
                                  const std::unordered_set<std::string_view>*     filter)
{
    methods_.clear();
    fields_.clear();
    types_.clear();
    container.visitResources([&](std::string_view uri, const model::IResource& resource) {
        if (filter != nullptr && filter->find(uri) == filter->end()) return;
        const auto* classResource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (!classResource) return;
        // 净化预检与 WorkspaceBase::validateCandidate 同口径：引用语义损坏的
        // class 不进扫描（畸形 class 的跳过纪律）。
        if (!spi::sanitizeClassFile(classResource->get()).empty()) return;
        auto session = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(classResource->get()));
        if (session.is_err()) return;
        auto held = std::move(session).unwrap();

        const auto sourceClass = utf8(held->backing().thisName());

        // class 头声明：super 与直接接口是 class 级类型引用（无源方法）。
        const auto super = held->backing().superName();
        if (!super.empty()) types_.push_back({utf8(super), {sourceClass.clone(), {}, {}, -1}});
        for (const auto interfaceCp : held->backing().interfaces) {
            const auto name = held->backing().className(interfaceCp);
            if (!name.empty()) types_.push_back({utf8(name), {sourceClass.clone(), {}, {}, -1}});
        }

        for (auto& visit : jbc::ir::collectCodeUnits(*held)) {
            if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
            const jbc::ir::CodeUnit& code = *visit.unit;
            const auto&            unit  = held->unit().methods[visit.methodIdx];
            const ReferenceSite    site{sourceClass.clone(),
                                        utf8(held->unit().symbols().str(unit.name)),
                                        utf8(held->unit().symbols().str(unit.desc)), -1};

            const auto& order = code.dom.order();
            for (ca::usize index = 0; index < order.size(); ++index) {
                const auto          insnId = order[index];
                const jbc::ir::Insn* insn  = code.dom.peek(insnId);
                if (!insn) continue;
                ReferenceSite at = site.clone();
                at.insnIndex     = static_cast<ca::i32>(index);

                if (isInvokeOp(insn->op)) {
                    jbc::ir::ClassSession::MemberSem sem;
                    if (held->memberOf(*insn, sem) &&
                        sem.kind != jbc::ir::ClassSession::MemberSem::Kind::InvokeDynamic &&
                        !sem.owner.empty()) {
                        methods_.push_back({{utf8(sem.owner), utf8(sem.name), utf8(sem.desc)},
                                            at.clone(),
                                            MethodReferenceKind::Direct});
                        types_.push_back({utf8(sem.owner), at.clone()});
                    }
                    continue;
                }
                if (isFieldAccessOp(insn->op)) {
                    jbc::ir::ClassSession::MemberSem sem;
                    if (held->memberOf(*insn, sem) && !sem.owner.empty()) {
                        fields_.push_back({{utf8(sem.owner), utf8(sem.name), utf8(sem.desc)},
                                           at.clone(),
                                           FieldReferenceKind::Direct});
                        types_.push_back({utf8(sem.owner), at.clone()});
                    }
                    continue;
                }
                if (isTypeOp(insn->op)) {
                    if (insn->op == Op::MULTIANEWARRAY) {
                        // multianewarray 的 cp 指向数组描述符；提取元素对象类型。
                        const auto element = support::objectClassFromArrayDescriptor(
                            analysis::resolveClassName(held->backing().cp, insn->cp));
                        if (!element.empty()) types_.push_back({utf8(element), at.clone()});
                        continue;
                    }
                    const auto name = held->classOf(*insn);
                    if (!name.empty()) types_.push_back({utf8(name), at.clone()});
                    continue;
                }
                if (insn->op == Op::LDC || insn->op == Op::LDC_W) {
                    jbc::ir::ClassSession::ConstSem constant;
                    if (!held->constOf(*insn, constant)) continue;
                    if (constant.kind == jbc::ir::ClassSession::ConstSem::Kind::Class &&
                        !constant.str.empty()) {
                        // 数组形态的 class 常量提取元素对象类型（LDC "[Lfoo/Bar;" → foo/Bar）。
                        const auto element = support::objectClassFromArrayDescriptor(constant.str);
                        types_.push_back({element.empty() ? utf8(constant.str) : utf8(element),
                                          at.clone()});
                        continue;
                    }
                    // class-string 元数据事实：形态合法的字符串常量是反射引用候选
                    //（与 remapper 反射改写候选同一语法，remap 改的 xref 必须可见）。
                    //toInternalReflectionName 0.0.28 起直接产出 Utf8String，原值移交。
                    if (constant.kind == jbc::ir::ClassSession::ConstSem::Kind::String &&
                        asm_remapper::looksLikeClassName(constant.str)) {
                        types_.push_back(
                            {asm_remapper::toInternalReflectionName(constant.str),
                             at.clone()});
                    }
                    continue;
                }
            }
        }
    });
}

std::vector<MethodReference> ReferenceScan::methodReferencesTo(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor) const
{
    std::vector<MethodReference> result;
    for (const auto& reference : methods_) {
        if (reference.target.owner == owner && reference.target.name == name &&
            reference.target.descriptor == descriptor) {
            result.push_back(reference.clone());
        }
    }
    return result;
}

std::vector<MethodReference> ReferenceScan::methodReferencesFrom(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor) const
{
    std::vector<MethodReference> result;
    for (const auto& reference : methods_) {
        if (reference.site.sourceClass == owner && reference.site.sourceMethod == name &&
            reference.site.sourceDescriptor == descriptor) {
            result.push_back(reference.clone());
        }
    }
    return result;
}

std::vector<MethodReference> ReferenceScan::methodReferencesFromClass(
    const ca::str::Utf8StringRef& owner) const
{
    std::vector<MethodReference> result;
    for (const auto& reference : methods_) {
        if (reference.site.sourceClass == owner) result.push_back(reference.clone());
    }
    return result;
}

std::vector<FieldReference> ReferenceScan::fieldReferencesTo(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor) const
{
    std::vector<FieldReference> result;
    for (const auto& reference : fields_) {
        if (reference.target.owner == owner && reference.target.name == name &&
            reference.target.descriptor == descriptor) {
            result.push_back(reference.clone());
        }
    }
    return result;
}

std::vector<FieldReference> ReferenceScan::fieldReferencesFromClass(
    const ca::str::Utf8StringRef& owner) const
{
    std::vector<FieldReference> result;
    for (const auto& reference : fields_) {
        if (reference.site.sourceClass == owner) result.push_back(reference.clone());
    }
    return result;
}

std::vector<TypeReference> ReferenceScan::typeReferencesTo(const ca::str::Utf8StringRef& targetType) const
{
    std::vector<TypeReference> result;
    for (const auto& reference : types_) {
        if (reference.targetType == targetType) result.push_back(reference.clone());
    }
    return result;
}

std::vector<TypeReference> ReferenceScan::typeReferencesFromClass(
    const ca::str::Utf8StringRef& owner) const
{
    std::vector<TypeReference> result;
    for (const auto& reference : types_) {
        if (reference.site.sourceClass == owner) result.push_back(reference.clone());
    }
    return result;
}

std::string methodReferenceKind(MethodReferenceKind)
{
    return "direct";
}

std::string fieldReferenceKind(FieldReferenceKind)
{
    return "direct";
}

ReferenceInfo referenceInfo(const ReferenceSite& site, std::string_view target, std::string_view kind)
{
    // ReferenceInfo DTO 文本字段换型 Utf8String：site 切片按 clone 复制，
    // target/kind 视图按 from_data_unchecked 字节直通（analysis_ingest utf8() 同口径）。
    return {site.sourceClass.clone(), site.sourceMethod.clone(), site.sourceDescriptor.clone(),
            site.insnIndex, ca::str::Utf8String::from_data_unchecked(
                                reinterpret_cast<const ca::u8*>(target.data()), target.size()),
            ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(kind.data()),
                                            kind.size())};
}

void sortReferences(std::vector<ReferenceInfo>& references)
{
    std::sort(references.begin(), references.end(), [](const ReferenceInfo& left,
                                                       const ReferenceInfo& right) {
        if (left.sourceClass != right.sourceClass) return left.sourceClass < right.sourceClass;
        if (left.sourceMethod != right.sourceMethod) return left.sourceMethod < right.sourceMethod;
        if (left.sourceDescriptor != right.sourceDescriptor)
            return left.sourceDescriptor < right.sourceDescriptor;
        if (left.instructionIndex != right.instructionIndex)
            return left.instructionIndex < right.instructionIndex;
        if (left.kind != right.kind) return left.kind < right.kind;
        return left.target < right.target;
    });
}

}   // namespace mjt::workspace
