/// @file        dex_domain.cpp
/// @brief       dex 域实现：装载、类事实、引用扫描、常量事实与文本视图。
///
/// 容错纪律 fail-open：单类/单方法解码失败跳过（与 JVM 扫描的畸形 class
/// 跳过纪律一致）；裸 dex 整文件解析失败与 apk 无根级 dex 条目是装载级
/// 错误（fail-closed）。文本统一字节直通（MUTF-8 视图 → from_data_unchecked，
/// 与 jbc 面文本同口径）；描述符语法与 JVM 同形，仅类级身份做 "L…;" →
/// 内名换算（方法/字段描述符零转换）。

#include "dex_domain.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

#include "dex/dex.hpp"
#include "jstr/conversion.hpp"
#include "smali/assemble.hpp"
#include "smali/disassemble.hpp"

#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/jasm/classfile_utf8.hpp"
#include "mjbase/jar/jar_reader.hpp"

#include "classfile_support.hpp"
#include "constant_scan.hpp"
#include "search_match.hpp"

namespace mjt::workspace {

namespace {

using namespace dex::core;
using ca::str::Utf8String;
using MUtf8StringRef = jstr::MUtf8StringRef;   ///< libdex 懒取文本视图（ns jstr）
using mjbase::project::DbNumberKind;
using mjbase::project::DbNumberRecord;
using mjbase::project::DbStringKind;
using mjbase::project::DbStringRecord;

/// @brief dex 显式数组类型指令（元素提取口径，multianewarray 同款）。
constexpr ca::u8 kOpConstClass        = 0x1c;
constexpr ca::u8 kOpNewArray          = 0x23;
constexpr ca::u8 kOpFilledNewArray    = 0x24;
constexpr ca::u8 kOpFilledNewArrayR   = 0x25;
/// @brief const 字面量家族（数值切片口径；const-wide 族记 Long）。
constexpr ca::u8 kOpConst4            = 0x12;
constexpr ca::u8 kOpConstWideHigh16   = 0x19;
constexpr ca::u8 kOpConstWide16       = 0x16;
/// @brief 可 CHA 展开的 invoke 操作码（虚/接口及其 range 变体）。
constexpr ca::u8 kOpInvokeVirtualR    = 0x74;
constexpr ca::u8 kOpInvokeInterfaceR  = 0x78;

/// @brief 底层文本（MUTF-8 视图/string_view）→ 拥有型 Utf8String：字节原文
///        直通，不做二次校验（analysis_ingest utf8() 同口径）。
Utf8String utf8(std::string_view value)
{
    return Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(value.data()),
                                           value.size());
}

/// @brief 懒取文本兜底：取值失败（BadIndex 等）回空串——fail-open 纪律。
std::string textOf(const Result<MUtf8StringRef, ReadError>& value)
{
    if (value.is_err()) return {};
    const MUtf8StringRef view = value.unwrap();
    return std::string(view.data(), view.size());
}

/// @brief 类/类型描述符（"Lcom/foo/Bar;"）→ JVM internal name（"com/foo/Bar"）。
///        非 "L…;" 形态（原始类型/数组）原样返回——数组形态的元素提取走
///        objectClassFromArrayDescriptor。
std::string internalNameOfDescriptor(std::string_view descriptor)
{
    if (descriptor.size() >= 2 && descriptor.front() == 'L' && descriptor.back() == ';') {
        return std::string(descriptor.substr(1, descriptor.size() - 2));
    }
    return std::string(descriptor);
}

/// @brief dex 访问旗标 → JVM 语义位（低 16 位直通；0x10000 构造器/
///        0x20000 declared-synchronized 高位不进 JVM 面——构造器语义已由
///        <init> 描述符承载，与 JVM 同形）。
ca::u32 accessFlagsOf(ca::u32 dexFlags) noexcept { return dexFlags & 0xFFFFu; }

/// @brief u64 位型 → float/double（静态初值 Float/Double 行装配口径）。
float bitsToFloat(ca::u64 bits) noexcept
{
    const ca::u32 raw = static_cast<ca::u32>(bits & 0xFFFFFFFFu);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}
double bitsToDouble(ca::u64 bits) noexcept
{
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// @brief 方法 proto（proto_ids 条目）→ JVM 方法描述符 "(参数)返回"。
std::string protoDescriptorOf(DexFile& dex, ca::u32 protoIdx)
{
    if (protoIdx >= dex.protoIds.size()) return {};
    const ProtoId& proto = dex.protoIds[protoIdx];
    const auto returnType = textOf(dex.typeDescriptor(proto.returnTypeIdx));
    auto parameters = dex.protoParameters(protoIdx);
    if (parameters.is_err() || returnType.empty()) return {};
    std::string descriptor = "(";
    const TypeListSpan& span = parameters.unwrap();
    for (ca::u32 i = 0; i < span.size; ++i) {
        const auto type = textOf(dex.typeDescriptor(span.at(i)));
        if (type.empty()) return {};
        descriptor += type;
    }
    descriptor += ")";
    descriptor += returnType;
    return descriptor;
}

std::string methodDescriptorOf(DexFile& dex, ca::u32 methodIdx)
{
    if (methodIdx >= dex.methodIds.size()) return "()";
    auto descriptor = protoDescriptorOf(dex, dex.methodIds[methodIdx].protoIdx);
    return descriptor.empty() ? "()" : std::move(descriptor);
}

/// @brief 方法身份三元组（owner 为 internal name）；越界回空（调用方跳过）。
struct MethodKey
{
    std::string owner;
    std::string name;
    std::string descriptor;

    bool valid() const noexcept { return !owner.empty() && !name.empty(); }
};

MethodKey methodKeyOf(DexFile& dex, ca::u32 methodIdx)
{
    if (methodIdx >= dex.methodIds.size()) return {};
    MethodKey key;
    key.owner      = internalNameOfDescriptor(textOf(dex.methodClassDescriptor(methodIdx)));
    key.name       = textOf(dex.methodName(methodIdx));
    key.descriptor = methodDescriptorOf(dex, methodIdx);
    return key;
}

/// @brief 字段身份三元组（owner 为 internal name；descriptor = 字段描述符）。
struct FieldKey
{
    std::string owner;
    std::string name;
    std::string descriptor;

    bool valid() const noexcept { return !owner.empty() && !name.empty(); }
};

FieldKey fieldKeyOf(DexFile& dex, ca::u32 fieldIdx)
{
    if (fieldIdx >= dex.fieldIds.size()) return {};
    FieldKey key;
    key.owner      = internalNameOfDescriptor(textOf(dex.fieldClassDescriptor(fieldIdx)));
    key.name       = textOf(dex.fieldName(fieldIdx));
    key.descriptor = textOf(dex.fieldTypeDescriptor(fieldIdx));
    return key;
}

/// @brief 一个方法的解码体：insns 程序序（payload 伪指令占一席，与 smali
///        渲染与 ordinal 口径一致）；解码失败 = 空体（fail-open）。
struct DecodedBody
{
    std::vector<Insn> insns;
};

DecodedBody decodeMethodBody(DexFile& dex, const EncodedMethod& method)
{
    DecodedBody body;
    if (method.codeOff == 0) return body;
    auto code = dex.codeItem(method.codeOff);
    if (code.is_err()) return body;
    const CodeItem* item = code.unwrap();
    auto decoded =
        decodeInsns(subOrEmpty(dex.source(), item->insns.offset, item->insns.length));
    if (decoded.is_err()) return body;
    body.insns = std::move(decoded).unwrap();
    return body;
}

/// @brief 数组描述符 → 元素对象类（"[[LFoo;" → "Foo"）；原始数组/非数组回空。
std::string elementClassOf(std::string_view arrayDescriptor)
{
    return support::objectClassFromArrayDescriptor(arrayDescriptor);
}

/// @brief 引用扫描结果（buildReferenceScan 与引用查询共用）。
struct RefIndex
{
    std::vector<MethodReference> methods;
    std::vector<FieldReference>  fields;
    std::vector<TypeReference>   types;
    std::vector<DynamicReference> dynamicCalls;
};

/// @brief DEX method_handle_item tags for field access and method invocation.
constexpr ca::u16 kLastFieldHandle = 0x03;
constexpr ca::u16 kInvokeStaticHandle = 0x04;
constexpr ca::u16 kLastMethodHandle = 0x08;

const EncodedArrayItem* callSiteOf(DexFile& dex, ca::u32 index)
{
    auto decoded = dex.callSiteValues(index);
    if (decoded.is_err() || decoded.unwrap() == nullptr) return nullptr;
    const auto* site = decoded.unwrap();
    const auto& values = site->values;
    if (values.size() < 3 || values[0].type != EncodedValueType::MethodHandle ||
        values[1].type != EncodedValueType::String ||
        values[2].type != EncodedValueType::MethodType ||
        values[0].index >= dex.methodHandles.size() ||
        dex.methodHandles[values[0].index].handleType != kInvokeStaticHandle ||
        values[1].index >= dex.stringCount() || values[2].index >= dex.protoIds.size())
        return nullptr;
    return site;
}

std::string callSiteTargetText(DexFile& dex, const EncodedArrayItem& site)
{
    const auto name = textOf(dex.string(site.values[1].index));
    const auto descriptor = protoDescriptorOf(dex, site.values[2].index);
    if (name.empty() || descriptor.empty()) return {};
    return name + descriptor;
}

void addMethodReference(RefIndex& out, const MethodKey& target, const ReferenceSite& at,
                        MethodReferenceKind kind)
{
    if (!target.valid()) return;
    out.methods.push_back({{utf8(target.owner), utf8(target.name), utf8(target.descriptor)},
                           at.clone(), kind});
    out.types.push_back({utf8(target.owner), at.clone()});
}

void addFieldReference(RefIndex& out, const FieldKey& target, const ReferenceSite& at,
                       FieldReferenceKind kind)
{
    if (!target.valid()) return;
    out.fields.push_back({{utf8(target.owner), utf8(target.name), utf8(target.descriptor)},
                          at.clone(), kind});
    out.types.push_back({utf8(target.owner), at.clone()});
}

void addHandleReference(DexFile& dex, ca::u32 index, const ReferenceSite& at, RefIndex& out,
                        MethodReferenceKind methodKind)
{
    if (index >= dex.methodHandles.size()) return;
    const auto& handle = dex.methodHandles[index];
    if (handle.handleType <= kLastFieldHandle) {
        addFieldReference(out, fieldKeyOf(dex, handle.targetIdx), at,
                          FieldReferenceKind::MethodHandle);
    }
    else if (handle.handleType <= kLastMethodHandle) {
        addMethodReference(out, methodKeyOf(dex, handle.targetIdx), at, methodKind);
    }
}

void scanCallSiteReferences(DexFile& dex, ca::u32 index, const ReferenceSite& at, RefIndex& out)
{
    const auto* site = callSiteOf(dex, index);
    if (site == nullptr) return;
    const auto target = callSiteTargetText(dex, *site);
    if (!target.empty()) out.dynamicCalls.push_back({utf8(target), at.clone()});
    addHandleReference(dex, site->values.front().index, at, out, MethodReferenceKind::Bootstrap);
    for (ca::usize i = 3; i < site->values.size(); ++i) {
        const auto& arg = site->values[i];
        if (arg.type == EncodedValueType::MethodHandle)
            addHandleReference(dex, arg.index, at, out, MethodReferenceKind::MethodHandle);
    }
}

/// @brief 单类引用扫描：类头声明（super/interfaces，class 级引用 insnIndex=-1）
///        + 逐方法指令事实（语义逐条对齐 reference_scan.cpp 的 JVM 扫描）。
void scanClassRefs(DexFile& dex, ca::u32 classDefIdx, const std::string& sourceClass,
                   RefIndex& out)
{
    // class 头声明：super 与直接接口是 class 级类型引用（无源方法）。
    const std::string super =
        internalNameOfDescriptor(textOf(dex.superclassDescriptor(classDefIdx)));
    if (!super.empty()) out.types.push_back({utf8(super), {utf8(sourceClass), {}, {}, -1}});
    auto interfaces = dex.interfaces(classDefIdx);
    if (interfaces.is_ok()) {
        const TypeListSpan& span = interfaces.unwrap();
        for (ca::u32 i = 0; i < span.size; ++i) {
            const std::string name =
                internalNameOfDescriptor(textOf(dex.typeDescriptor(span.at(i))));
            if (!name.empty()) out.types.push_back({utf8(name), {utf8(sourceClass), {}, {}, -1}});
        }
    }

    auto data = dex.classData(classDefIdx);
    if (data.is_err() || data.unwrap() == nullptr) return;
    const ClassData* classData = data.unwrap();
    for (const auto* group : {&classData->directMethods, &classData->virtualMethods}) {
        for (const EncodedMethod& method : *group) {
            const MethodKey site = methodKeyOf(dex, method.methodIndex);
            if (!site.valid()) continue;
            const DecodedBody body = decodeMethodBody(dex, method);
            for (ca::u32 index = 0; index < body.insns.size(); ++index) {
                const Insn& insn = body.insns[index];
                const ReferenceSite at{utf8(sourceClass), utf8(site.name),
                                       utf8(site.descriptor), static_cast<ca::i32>(index)};
                switch (insn.indexKind) {
                case IndexKind::Method: {
                    // invoke 家族（含 range/polymorphic）：方法引用 + owner
                    // 类型引用。45cc/4rcc 的 protoIndex（MethodType 面）不建模。
                    addMethodReference(out, methodKeyOf(dex, insn.index), at,
                                       MethodReferenceKind::Direct);
                    break;
                }
                case IndexKind::Field: {
                    // iget/iput/sget/sput 家族：字段访问 + owner 类型引用。
                    addFieldReference(out, fieldKeyOf(dex, insn.index), at,
                                      FieldReferenceKind::Direct);
                    break;
                }
                case IndexKind::Type: {
                    if (insn.opcode == kOpNewArray || insn.opcode == kOpFilledNewArray ||
                        insn.opcode == kOpFilledNewArrayR) {
                        // 数组类型指令：提取元素对象类（multianewarray 同款；
                        // 原始数组无对象元素，不产引用）。
                        const std::string element =
                            elementClassOf(textOf(dex.typeDescriptor(insn.index)));
                        if (!element.empty()) out.types.push_back({utf8(element), at.clone()});
                        break;
                    }
                    const std::string name =
                        internalNameOfDescriptor(textOf(dex.typeDescriptor(insn.index)));
                    if (name.empty()) break;
                    if (insn.opcode == kOpConstClass) {
                        // const-class 数组形态（LDC Class 数组同款）：对象数组提
                        // 元素，其余（含原始数组形态）原样。
                        const std::string element = elementClassOf(name);
                        out.types.push_back({utf8(element.empty() ? name : element), at.clone()});
                        break;
                    }
                    out.types.push_back({utf8(name), at.clone()});
                    break;
                }
                case IndexKind::String: {
                    // const-string 家族：形态合法类名是反射引用候选（remapper
                    // 反射语法同款，remap 改的 xref 必须可见）。
                    const std::string value = textOf(dex.string(insn.index));
                    if (asm_remapper::looksLikeClassName(value)) {
                        out.types.push_back({asm_remapper::toInternalReflectionName(value),
                                             at.clone()});
                    }
                    break;
                }
                case IndexKind::CallSite:
                    scanCallSiteReferences(dex, insn.index, at, out);
                    break;
                case IndexKind::MethodHandle:
                    addHandleReference(dex, insn.index, at, out,
                                       MethodReferenceKind::MethodHandle);
                    break;
                case IndexKind::None:
                case IndexKind::Proto:
                default:
                    break;
                }
            }
        }
    }
}

/// @brief 全域引用扫描（跨 dex 文件的 class_def 序）。
//@brief 方法引用目标文本（引擎口径：owner + "." + name + descriptor）。
std::string methodTargetText(const MethodKey& key)
{
    return key.owner + "." + key.name + key.descriptor;
}

/// @brief 引用三桶 → ReferenceInfo 精确目标查询（xrefsTo*/calleesOfMethod
///        共用；行序 sortReferences 收口，无 supporting 标注）。目标文本
///        引擎口径：方法/字段 owner + "." + name + descriptor，类型为内名。
std::vector<ReferenceInfo> collectReferenceInfos(
    const ReferenceScan& scan,
    const std::function<bool(const MethodReference&)>& methodMatch,
    const std::function<bool(const FieldReference&)>& fieldMatch,
    const std::function<bool(const TypeReference&)>& typeMatch)
{
    std::vector<ReferenceInfo> result;
    for (const MethodReference& reference : scan.methodReferences()) {
        if (!methodMatch(reference)) continue;
        result.push_back(referenceInfo(
            reference.site,
            reference.target.owner.to_std_string() + "." +
                reference.target.name.to_std_string() +
                reference.target.descriptor.to_std_string(),
            methodReferenceKind(reference.kind)));
    }
    for (const FieldReference& reference : scan.fieldReferences()) {
        if (!fieldMatch(reference)) continue;
        result.push_back(referenceInfo(
            reference.site,
            reference.target.owner.to_std_string() + "." +
                reference.target.name.to_std_string() +
                reference.target.descriptor.to_std_string(),
            fieldReferenceKind(reference.kind)));
    }
    for (const TypeReference& reference : scan.typeReferences()) {
        if (!typeMatch(reference)) continue;
        result.push_back(
            referenceInfo(reference.site, reference.targetType.to_std_string(), "type"));
    }
    sortReferences(result);
    result.erase(std::unique(result.begin(), result.end(),
                             [](const ReferenceInfo& left, const ReferenceInfo& right) {
                                 return left.sourceClass == right.sourceClass &&
                                        left.sourceMethod == right.sourceMethod &&
                                        left.sourceDescriptor == right.sourceDescriptor &&
                                        left.instructionIndex == right.instructionIndex &&
                                        left.kind == right.kind && left.target == right.target;
                             }), result.end());
    return result;
}

/// @brief dex 访问旗标 → MethodInfo/FieldInfo 装配。
MethodInfo methodInfoOf(DexFile& dex, const EncodedMethod& method, bool includeBody = true)
{
    MethodInfo info;
    const MethodKey key = methodKeyOf(dex, method.methodIndex);
    info.name       = utf8(key.name);
    info.descriptor = utf8(key.descriptor);
    info.access     = accessFlagsOf(method.accessFlags);
    if (includeBody && method.codeOff != 0) {
        auto code = dex.codeItem(method.codeOff);
        if (code.is_ok()) {
            const CodeItem* item = code.unwrap();
            // dex 寄存器面 → JVM maxStack/maxLocals 近似：locals = 总寄存器
            // - 入参槽；stack = outs 槽。呈现层近似值，非验证语义。
            info.maxLocals =
                static_cast<ca::i32>(item->registersSize) - static_cast<ca::i32>(item->insSize);
            info.maxStack   = static_cast<ca::i32>(item->outsSize);
            info.tryCatchCount = item->tryBlocks.size();
            info.instructionCount = decodeMethodBody(dex, method).insns.size();
        }
    }
    return info;
}

FieldInfo fieldInfoOf(DexFile& dex, const EncodedField& field)
{
    FieldInfo info;
    const FieldKey key = fieldKeyOf(dex, field.fieldIndex);
    info.name       = utf8(key.name);
    info.descriptor = utf8(key.descriptor);
    info.access     = accessFlagsOf(field.accessFlags);
    return info;
}

/// @brief 静态初值 → ConstantValueInfo（Byte/Short/Int/Long/Char/Float/
///        Double/String；其余形态不进呈现面）。
std::optional<ConstantValueInfo> constantValueOf(DexFile& dex, const EncodedValue& value)
{
    ConstantValueInfo out;
    switch (value.type) {
    case EncodedValueType::Byte:
    case EncodedValueType::Short:
    case EncodedValueType::Int:
        out.kind = ConstantValueInfo::Kind::Integer;
        out.text = utf8(numberText(value.integer));
        return out;
    case EncodedValueType::Char:
        out.kind = ConstantValueInfo::Kind::Integer;
        out.text = utf8(numberText(static_cast<ca::i64>(value.bits)));
        return out;
    case EncodedValueType::Long:
        out.kind = ConstantValueInfo::Kind::Long;
        out.text = utf8(numberText(value.integer));
        return out;
    case EncodedValueType::Float:
        out.kind = ConstantValueInfo::Kind::Float;
        out.text = utf8(numberText(bitsToFloat(value.bits)));
        return out;
    case EncodedValueType::Double:
        out.kind = ConstantValueInfo::Kind::Double;
        out.text = utf8(numberText(bitsToDouble(value.bits)));
        return out;
    case EncodedValueType::String:
        out.kind = ConstantValueInfo::Kind::String;
        out.text = utf8(textOf(dex.string(value.index)));
        return out;
    default:
        return std::nullopt;
    }
}

/// @brief 类全量事实装配（dex DOM → ClassInfo；version = dex 版本）。
ClassInfo buildClassInfo(DexFile& dex, ca::u32 classDefIdx, const std::string& resourceUri,
                        bool includeMethodBodies = true)
{
    ClassInfo info;
    info.resourceUri  = utf8(resourceUri);
    info.internalName = utf8(internalNameOfDescriptor(textOf(dex.classDescriptor(classDefIdx))));
    info.superName    = utf8(internalNameOfDescriptor(textOf(dex.superclassDescriptor(classDefIdx))));
    info.access       = accessFlagsOf(dex.classDefs[classDefIdx].accessFlags);
    info.version      = static_cast<ca::i32>(dex.header.version);

    auto interfaces = dex.interfaces(classDefIdx);
    if (interfaces.is_ok()) {
        const TypeListSpan& span = interfaces.unwrap();
        for (ca::u32 i = 0; i < span.size; ++i) {
            info.interfaces.push_back(
                utf8(internalNameOfDescriptor(textOf(dex.typeDescriptor(span.at(i))))));
        }
    }

    auto data = dex.classData(classDefIdx);
    const ClassData* classData = data.is_ok() ? data.unwrap() : nullptr;
    if (classData == nullptr) return info;

    // 静态初值与 staticFields 按声明序对齐（dex 规范：encoded_array 只列
    // 前 N 个有值静态字段）。
    std::vector<const EncodedValue*> staticValues;
    auto values = dex.staticValues(classDefIdx);
    if (values.is_ok() && values.unwrap() != nullptr) {
        for (const EncodedValue& value : values.unwrap()->values) {
            staticValues.push_back(&value);
        }
    }
    for (ca::usize i = 0; i < classData->staticFields.size(); ++i) {
        FieldInfo field = fieldInfoOf(dex, classData->staticFields[i]);
        if (i < staticValues.size()) field.constantValue = constantValueOf(dex, *staticValues[i]);
        info.fields.push_back(std::move(field));
    }
    for (const EncodedField& field : classData->instanceFields) {
        info.fields.push_back(fieldInfoOf(dex, field));
    }
    for (const EncodedMethod& method : classData->directMethods) {
        info.methods.push_back(methodInfoOf(dex, method, includeMethodBodies));
    }
    for (const EncodedMethod& method : classData->virtualMethods) {
        info.methods.push_back(methodInfoOf(dex, method, includeMethodBodies));
    }
    return info;
}

/// @brief 静态初值 → 类级数值行（member_id=0 哨兵口径：insnIndex/opcode 恒 -1）。
std::vector<DbNumberRecord> staticValueNumberRecords(DexFile& dex, ca::u32 classDefIdx)
{
    std::vector<DbNumberRecord> records;
    auto values = dex.staticValues(classDefIdx);
    if (values.is_err() || values.unwrap() == nullptr) return records;
    for (const EncodedValue& value : values.unwrap()->values) {
        switch (value.type) {
        case EncodedValueType::Byte:
        case EncodedValueType::Short:
        case EncodedValueType::Char:
        case EncodedValueType::Int:
            records.push_back(dbIntNumberRecord(-1, -1, DbNumberKind::Int,
                                                value.type == EncodedValueType::Char
                                                    ? static_cast<ca::i64>(value.bits)
                                                    : value.integer));
            break;
        case EncodedValueType::Long:
            records.push_back(dbIntNumberRecord(-1, -1, DbNumberKind::Long, value.integer));
            break;
        case EncodedValueType::Float:
            records.push_back(dbFloat32NumberRecord(-1, -1, bitsToFloat(value.bits)));
            break;
        case EncodedValueType::Double:
            records.push_back(dbFloat64NumberRecord(-1, -1, bitsToDouble(value.bits)));
            break;
        default:
            break;   // 字符串/布尔/null/嵌套值不进数值面。
        }
    }
    return records;
}

/// @brief 指令行渲染（搜索口径）：助记符 + 索引目标/字面量/分支偏移；
///        寄存器不渲染（全文保真走 smaliText）。
std::string renderInsnLine(DexFile& dex, const Insn& insn)
{
    const OpcodeInfo& info = opcodeInfo(insn.opcode);
    std::string line = info.name;
    const auto appendIndex = [&](const std::string& text) {
        line += " " + text;
    };
    switch (insn.indexKind) {
    case IndexKind::Method: {
        const MethodKey key = methodKeyOf(dex, insn.index);
        if (key.valid()) appendIndex(key.owner + "->" + key.name + key.descriptor);
        break;
    }
    case IndexKind::Field: {
        const FieldKey key = fieldKeyOf(dex, insn.index);
        if (key.valid()) appendIndex(key.owner + "->" + key.name + ":" + key.descriptor);
        break;
    }
    case IndexKind::Type: {
        const std::string descriptor = textOf(dex.typeDescriptor(insn.index));
        if (!descriptor.empty()) appendIndex(internalNameOfDescriptor(descriptor));
        break;
    }
    case IndexKind::String: {
        const std::string value = textOf(dex.string(insn.index));
        if (!value.empty()) appendIndex("\"" + value + "\"");
        break;
    }
    default:
        break;
    }
    if (insn.indexKind == IndexKind::None) {
        if (insn.opcode >= kOpConst4 && insn.opcode <= kOpConstWideHigh16) {
            // const/const-wide 家族（0x12..0x19）：字面量进行文本。
            appendIndex(std::to_string(insn.literal));
        }
        else if (insn.payloadKind != PayloadKind::None) {
            line += " (payload)";
        }
        else if (insn.branchOffset != 0) {
            appendIndex(std::to_string(insn.branchOffset));
        }
    }
    return line;
}

ca::i32 dexBytePc(ca::i64 codeUnits)
{
    if (codeUnits < 0 || codeUnits > std::numeric_limits<ca::i32>::max() / 2) return -1;
    return static_cast<ca::i32>(codeUnits * 2);
}

void renderDexFlow(const std::vector<Insn>& insns, const Insn& insn, InstructionDetailInfo& row)
{
    if (!insn.isBranch()) return;
    const ca::i64 target = static_cast<ca::i64>(insn.address) + insn.branchOffset;
    if (insn.format != InsnFormat::F31t) {
        row.branchTargetPc = dexBytePc(target);
        return;
    }
    row.payloadPc = dexBytePc(target);
    const PayloadKind kind = switchKindOf(insn.opcode);
    if (kind != PayloadKind::PackedSwitch && kind != PayloadKind::SparseSwitch) return;

    row.isSwitch = true;
    row.switchDefaultPc = dexBytePc(static_cast<ca::i64>(insn.address) + insn.width);
    if (row.payloadPc < 0) return;
    const auto payload = std::lower_bound(insns.begin(), insns.end(), target,
                                         [](const Insn& item, ca::i64 address) {
                                             return item.address < address;
                                         });
    if (payload == insns.end() || payload->address != target || payload->payloadKind != kind)
        return;
    const bool packed = kind == PayloadKind::PackedSwitch;
    for (ca::u32 index = 0; index < payload->payloadCount; ++index) {
        const ca::i64 key = packed ? static_cast<ca::i64>(payload->payloadFirstKey) + index
                                   : payload->sparseSwitchKey(index);
        const ca::i64 offset = packed ? payload->packedSwitchTarget(index)
                                      : payload->sparseSwitchTarget(index);
        const ca::i32 targetPc = dexBytePc(static_cast<ca::i64>(insn.address) + offset);
        if (key <= std::numeric_limits<ca::i32>::max() && targetPc >= 0)
            row.switchCases.push_back({static_cast<ca::i32>(key), targetPc});
    }
}

void renderDexOperands(DexFile& dex, const Insn& insn, bool resolve, InstructionDetailInfo& row)
{
    std::string operands;
    const auto append = [&](const std::string& value) {
        if (!operands.empty()) operands += ", ";
        operands += value;
    };
    if (insn.format == InsnFormat::F3rc || insn.format == InsnFormat::F4rcc) {
        if (insn.registerCount > 0)
            append("v" + std::to_string(insn.startRegister) + " .. v" +
                   std::to_string(insn.startRegister + insn.registerCount - 1));
    }
    else {
        for (ca::u8 index = 0; index < std::min<ca::u8>(insn.registerCount, 5); ++index)
            append("v" + std::to_string(insn.registers[index]));
    }
    if (insn.indexKind != IndexKind::None) {
        row.cpIndex = static_cast<ca::i32>(insn.index);
        const auto line = renderInsnLine(dex, insn);
        const auto space = line.find(' ');
        std::string reference = "#" + std::to_string(insn.index);
        if (space != std::string::npos) {
            auto display = jstr::mutf8_to_utf8(
                MUtf8StringRef(std::string_view(line).substr(space + 1)),
                {jstr::ConversionValidationMode::kLenient, true});
            if (display.is_ok()) {
                reference = std::move(display).unwrap();
                if (resolve) row.operandComment = utf8(reference);
            }
        }
        append(reference);
    }
    else if (insn.isBranch()) {
        const ca::i32 targetPc = insn.format == InsnFormat::F31t ? row.payloadPc
                                                               : row.branchTargetPc;
        if (targetPc >= 0) append(std::to_string(targetPc));
    }
    else if ((insn.opcode >= kOpConst4 && insn.opcode <= kOpConstWideHigh16) ||
             insn.format == InsnFormat::F22b || insn.format == InsnFormat::F22s) {
        append(std::to_string(insn.literal));
    }
    else if (insn.isPayload()) {
        append(std::to_string(insn.payloadCount));
    }
    row.operandText = utf8(operands);
}

/// @brief jar 读错误的文本面（诊断消息用）。
std::string jarErrorText(mjbase::jar::JarError::Kind kind)
{
    switch (kind) {
    case mjbase::jar::JarError::Kind::FileNotFound: return "file not found";
    case mjbase::jar::JarError::Kind::OpenFailed:   return "open failed";
    case mjbase::jar::JarError::Kind::ReadFailed:   return "read failed";
    case mjbase::jar::JarError::Kind::WriteFailed:  return "write failed";
    case mjbase::jar::JarError::Kind::BadEntryName: return "bad entry name";
    }
    return "unknown";
}

}   // namespace

//-- 装载 --------------------------------------------------------------------

DexDomain::~DexDomain() = default;

WorkspaceResult<std::unique_ptr<DexDomain>> DexDomain::loadFromJarReader(
    mjbase::jar::IJarReader& reader)
{
    std::unique_ptr<DexDomain> domain(new DexDomain());
    auto read = reader.read([&](const Utf8String& name, const std::vector<ca::u8>& data) {
        const std::string entry = name.to_std_string();
        // 根级 *.dex 条目（classes*.dex 口径的一般化；子目录条目不取）。
        if (entry.size() < 4 || entry.rfind(".dex") != entry.size() - 4 ||
            entry.find('/') != std::string::npos) {
            return;
        }
        auto parsed = readDex(ca::core::ByteSlice(data.data(), data.size()), ReadOptions{});
        if (parsed.is_err()) {
            domain->dexLoadErrors_.push_back(utf8(entry + ": " + to_string(parsed.unwrap_err())));
            return;
        }
        domain->dexes_.push_back(std::move(parsed).unwrap());
        domain->dexUris_.push_back(entry);
    });
    if (read.is_err()) {
        return ca::core::Err(WorkspaceErrorInfo{
            WorkspaceError::DataLoss,
            "open apk: container read failed: " + jarErrorText(std::move(read).unwrap_err().kind)});
    }
    if (domain->dexes_.empty()) {
        const std::string detail = domain->dexLoadErrors_.empty()
                                       ? "no root *.dex entries (not an apk input)"
                                       : "dex parse failures: " + domain->dexLoadErrors_.front().to_std_string();
        return ca::core::Err(
            WorkspaceErrorInfo{WorkspaceError::InvalidArgument, "open apk: " + detail});
    }
    domain->dexVersion_ = domain->dexes_.front()->header.version;

    // 类索引（class_def 序；重名首见保留——跨 dex 重名不是合法多 dex 态，
    // 防御性跳过保证行序确定性）。
    for (ca::u32 dexIndex = 0; dexIndex < domain->dexes_.size(); ++dexIndex) {
        DexFile& dex = *domain->dexes_[dexIndex];
        for (ca::u32 classDefIdx = 0; classDefIdx < dex.classCount(); ++classDefIdx) {
            const std::string name =
                internalNameOfDescriptor(textOf(dex.classDescriptor(classDefIdx)));
            if (name.empty()) continue;
            if (domain->byName_.find(name) != domain->byName_.end()) continue;
            domain->byName_.emplace(name, DexDomain::Slot{dexIndex, classDefIdx});
            domain->classOrder_.push_back(name);
        }
    }
    return ca::core::Ok(std::move(domain));
}

WorkspaceResult<std::unique_ptr<DexDomain>> DexDomain::loadFromDexBytes(
    const std::vector<ca::u8>& bytes, const std::string& uri)
{
    std::unique_ptr<DexDomain> domain(new DexDomain());
    auto parsed = readDex(ca::core::ByteSlice(bytes.data(), bytes.size()), ReadOptions{});
    if (parsed.is_err()) {
        return ca::core::Err(WorkspaceErrorInfo{
            WorkspaceError::DataLoss,
            "open dex: " + std::string(to_string(parsed.unwrap_err()))});
    }
    domain->dexes_.push_back(std::move(parsed).unwrap());
    domain->dexUris_.push_back(uri);
    domain->dexVersion_ = domain->dexes_.front()->header.version;

    DexFile& dex = *domain->dexes_.front();
    for (ca::u32 classDefIdx = 0; classDefIdx < dex.classCount(); ++classDefIdx) {
        const std::string name =
            internalNameOfDescriptor(textOf(dex.classDescriptor(classDefIdx)));
        if (name.empty()) continue;
        if (domain->byName_.emplace(name, DexDomain::Slot{0, classDefIdx}).second)
            domain->classOrder_.push_back(name);
    }
    return ca::core::Ok(std::move(domain));
}

//-- 身份与寻址 ---------------------------------------------------------------

bool DexDomain::containsClass(const std::string& internalName) const
{
    return byName_.find(internalName) != byName_.end();
}

WorkspaceResult<std::vector<ca::u8>> DexDomain::assembleSmali(
    const std::string& internalName, std::string_view source) const
{
    const auto* slot = findClass(internalName);
    if (!slot) return ca::core::Err(notFound("assemble smali", internalName));

    auto original = writeDex(*dexes_[slot->dexIndex]);
    if (original.is_err())
        return ca::core::Err(domainError(WorkspaceError::DataLoss, "read original DEX",
                                          to_string(original.unwrap_err())));
    const auto bytes = std::move(original).unwrap();
    auto parsed = readDex(ca::core::ByteSlice(bytes.data(), bytes.size()), ReadOptions{});
    if (parsed.is_err())
        return ca::core::Err(domainError(WorkspaceError::DataLoss, "read original DEX",
                                          to_string(parsed.unwrap_err())));
    auto copy = std::move(parsed).unwrap();
    const auto originalClass = copy->classDefs[slot->classDefIndex].classIdx;
    // libdex removes a definition by erasing classDefs. Existing ID table references
    // remain valid, and the assembler reuses that class ID for the replacement.
    copy->classDefs.erase(copy->classDefs.begin() + slot->classDefIndex);
    dex::smali::SmaliAssembler assembler(*copy);
    auto assembled = assembler.assembleClass(source);
    if (assembled.is_err()) {
        const auto detail = std::move(assembled).unwrap_err();
        return ca::core::Err(domainError(
            WorkspaceError::InvalidArgument, "assemble smali",
            "line " + std::to_string(detail.line) + ": " + detail.detail));
    }
    if (copy->classDefs[assembled.unwrap()].classIdx != originalClass)
        return ca::core::Err(domainError(WorkspaceError::InvalidArgument, "assemble smali",
                                          "the replacement must keep the class descriptor"));

    auto rebuilt = buildDex(*copy, assembler.options());
    if (rebuilt.is_err())
        return ca::core::Err(domainError(WorkspaceError::DataLoss, "rebuild DEX",
                                          to_string(rebuilt.unwrap_err())));
    return ca::core::Ok(std::move(rebuilt).unwrap());
}

const DexDomain::Slot* DexDomain::findClass(const std::string& uriOrInternalName) const
{
    const auto byName = byName_.find(uriOrInternalName);
    if (byName != byName_.end()) return &byName->second;
    // URI 寻址：仅单类 dex（uri 唯一对应一个 class_def）。
    for (ca::u32 dexIndex = 0; dexIndex < dexes_.size(); ++dexIndex) {
        if (dexUris_[dexIndex] != uriOrInternalName) continue;
        if (dexes_[dexIndex]->classCount() == 1) {
            // 单类 dex 的槽从索引反查（classOrder_ 只收录成功索引的类）。
            for (const auto& [name, slot] : byName_) {
                if (slot.dexIndex == dexIndex) return &slot;
            }
        }
        return nullptr;
    }
    return nullptr;
}

WorkspaceErrorInfo DexDomain::domainError(WorkspaceError code, const std::string& operation,
                                          const std::string& detail) const
{
    return WorkspaceErrorInfo{code, operation + ": " + detail};
}

WorkspaceErrorInfo DexDomain::notFound(const std::string& operation,
                                       const std::string& target) const
{
    return domainError(WorkspaceError::NotFound, operation, target);
}

bool DexDomain::isSubclassInDomain(const std::string& candidate, const std::string& ancestor) const
{
    std::set<std::string> visited;
    std::string current = candidate;
    while (!current.empty() && visited.insert(current).second) {
        if (current == ancestor) return true;
        if (!containsClass(current)) return false;   // 域外截断。
        const Slot& slot = byName_.at(current);
        DexFile& dex = *dexes_[slot.dexIndex];
        current =
            internalNameOfDescriptor(textOf(dex.superclassDescriptor(slot.classDefIndex)));
    }
    return false;
}

//-- 类事实面 ------------------------------------------------------------------

WorkspaceResult<std::vector<ClassSummary>> DexDomain::listClassSummaries() const
{
    std::vector<ClassSummary> result;
    result.reserve(classOrder_.size());
    for (const std::string& name : classOrder_) {
        const Slot& slot = byName_.at(name);
        DexFile& dex = *dexes_[slot.dexIndex];
        ClassSummary summary;
        summary.resourceUri  = utf8(dexUris_[slot.dexIndex]);
        summary.internalName = utf8(name);
        summary.superName    = utf8(internalNameOfDescriptor(textOf(dex.superclassDescriptor(slot.classDefIndex))));
        summary.access       = accessFlagsOf(dex.classDefs[slot.classDefIndex].accessFlags);
        summary.version      = static_cast<ca::i32>(dex.header.version);
        auto interfaces = dex.interfaces(slot.classDefIndex);
        summary.interfaceCount = interfaces.is_ok() ? interfaces.unwrap().size : 0;
        auto data = dex.classData(slot.classDefIndex);
        const ClassData* classData = data.is_ok() ? data.unwrap() : nullptr;
        if (classData != nullptr) {
            summary.fieldCount =
                classData->staticFields.size() + classData->instanceFields.size();
            summary.methodCount =
                classData->directMethods.size() + classData->virtualMethods.size();
        }
        result.push_back(std::move(summary));
    }
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<std::vector<ClassInfo>> DexDomain::listClasses(bool includeMethodBodies) const
{
    std::vector<ClassInfo> result;
    result.reserve(classOrder_.size());
    for (const std::string& name : classOrder_) {
        const Slot& slot = byName_.at(name);
        result.push_back(buildClassInfo(*dexes_[slot.dexIndex], slot.classDefIndex,
                                        dexUris_[slot.dexIndex], includeMethodBodies));
    }
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<ClassInfo> DexDomain::classInfo(const std::string& uriOrInternalName) const
{
    const Slot* slot = findClass(uriOrInternalName);
    if (slot == nullptr) return ca::core::Err(notFound("class info", uriOrInternalName));
    return ca::core::Ok(
        buildClassInfo(*dexes_[slot->dexIndex], slot->classDefIndex, dexUris_[slot->dexIndex]));
}

WorkspaceResult<MethodInfo> DexDomain::methodInfo(const std::string& owner,
                                                  const std::string& name,
                                                  const std::string& descriptor) const
{
    const Slot* slot = findClass(owner);
    if (slot == nullptr) return ca::core::Err(notFound("method info", owner));
    DexFile& dex = *dexes_[slot->dexIndex];
    auto data = dex.classData(slot->classDefIndex);
    if (data.is_err() || data.unwrap() == nullptr) {
        return ca::core::Err(notFound("method info", owner + "." + name + descriptor));
    }
    const ClassData* classData = data.unwrap();
    for (const auto* group : {&classData->directMethods, &classData->virtualMethods}) {
        for (const EncodedMethod& method : *group) {
            const MethodKey key = methodKeyOf(dex, method.methodIndex);
            if (key.name == name && key.descriptor == descriptor) {
                return ca::core::Ok(methodInfoOf(dex, method));
            }
        }
    }
    return ca::core::Err(notFound("method info", owner + "." + name + descriptor));
}

WorkspaceResult<FieldInfo> DexDomain::fieldInfo(const std::string& owner,
                                                const std::string& name,
                                                const std::string& descriptor) const
{
    const Slot* slot = findClass(owner);
    if (slot == nullptr) return ca::core::Err(notFound("field info", owner));
    DexFile& dex = *dexes_[slot->dexIndex];
    auto data = dex.classData(slot->classDefIndex);
    if (data.is_err() || data.unwrap() == nullptr) {
        return ca::core::Err(notFound("field info", owner + "." + name + descriptor));
    }
    const ClassData* classData = data.unwrap();
    for (const auto* group : {&classData->staticFields, &classData->instanceFields}) {
        for (const EncodedField& field : *group) {
            const FieldKey key = fieldKeyOf(dex, field.fieldIndex);
            if (key.name == name && key.descriptor == descriptor) {
                return ca::core::Ok(fieldInfoOf(dex, field));
            }
        }
    }
    return ca::core::Err(notFound("field info", owner + "." + name + descriptor));
}

WorkspaceResult<MethodBytecode> DexDomain::methodBytecode(const std::string& owner,
                                                          const std::string& name,
                                                          const std::string& descriptor) const
{
    const Slot* slot = findClass(owner);
    if (slot == nullptr) return ca::core::Err(notFound("method bytecode", owner));
    DexFile& dex = *dexes_[slot->dexIndex];
    auto data = dex.classData(slot->classDefIndex);
    if (data.is_err() || data.unwrap() == nullptr) {
        return ca::core::Err(notFound("method bytecode", owner + "." + name + descriptor));
    }
    const ClassData* classData = data.unwrap();
    for (const auto* group : {&classData->directMethods, &classData->virtualMethods}) {
        for (const EncodedMethod& method : *group) {
            const MethodKey key = methodKeyOf(dex, method.methodIndex);
            if (key.name != name || key.descriptor != descriptor) continue;
            MethodBytecode out;
            out.method = methodInfoOf(dex, method);
            const DecodedBody body = decodeMethodBody(dex, method);
            if (body.insns.empty() && method.codeOff != 0) {
                // 方法体解码失败：fail-open 纪律回空指令面（method 元数据仍在）。
                return ca::core::Ok(std::move(out));
            }
            out.instructions.reserve(body.insns.size());
            for (ca::u32 index = 0; index < body.insns.size(); ++index) {
                const Insn& insn = body.insns[index];
                InstructionInfo row;
                row.index    = static_cast<ca::i32>(index);
                row.opcode   = static_cast<ca::i32>(insn.opcode);
                row.mnemonic = utf8(opcodeInfo(insn.opcode).name);
                row.nodeType = utf8(to_string(insn.format));
                out.instructions.push_back(std::move(row));
            }
            return ca::core::Ok(std::move(out));
        }
    }
    return ca::core::Err(notFound("method bytecode", owner + "." + name + descriptor));
}

WorkspaceResult<MethodInstructions> DexDomain::methodInstructions(const std::string& owner,
                                                                   const std::string& name,
                                                                   const std::string& descriptor,
                                                                   bool resolve) const
{
    const Slot* slot = findClass(owner);
    if (slot == nullptr) return ca::core::Err(notFound("method instructions", owner));
    DexFile& dex = *dexes_[slot->dexIndex];
    auto data = dex.classData(slot->classDefIndex);
    if (data.is_err() || data.unwrap() == nullptr) {
        return ca::core::Err(notFound("method instructions", owner + "." + name + descriptor));
    }
    const ClassData* classData = data.unwrap();
    for (const auto* group : {&classData->directMethods, &classData->virtualMethods}) {
        for (const EncodedMethod& method : *group) {
            const MethodKey key = methodKeyOf(dex, method.methodIndex);
            if (key.name != name || key.descriptor != descriptor) continue;
            MethodInstructions out;
            if (method.codeOff != 0) {
                auto code = dex.codeItem(method.codeOff);
                if (code.is_ok()) {
                    const CodeItem* item = code.unwrap();
                    out.maxLocals = static_cast<ca::i32>(item->registersSize) -
                                    static_cast<ca::i32>(item->insSize);
                    out.maxStack = static_cast<ca::i32>(item->outsSize);
                }
            }
            const DecodedBody body = decodeMethodBody(dex, method);
            out.instructions.reserve(body.insns.size());
            for (ca::u32 index = 0; index < body.insns.size(); ++index) {
                const Insn& insn = body.insns[index];
                InstructionDetailInfo row;
                row.pc       = dexBytePc(insn.address);
                row.opcode   = static_cast<ca::i32>(insn.opcode);
                row.mnemonic = utf8(opcodeInfo(insn.opcode).name);
                row.isPayload = insn.isPayload();
                renderDexFlow(body.insns, insn, row);
                renderDexOperands(dex, insn, resolve, row);
                out.instructions.push_back(std::move(row));
            }
            return ca::core::Ok(std::move(out));
        }
    }
    return ca::core::Err(notFound("method instructions", owner + "." + name + descriptor));
}

WorkspaceResult<std::vector<ConstantPoolEntryInfo>> DexDomain::constantPool(
    const std::string& uriOrInternalName, bool resolve) const
{
    const Slot* slot = findClass(uriOrInternalName);
    if (slot == nullptr) return ca::core::Err(notFound("constant pool", uriOrInternalName));
    DexFile& dex = *dexes_[slot->dexIndex];
    std::vector<ConstantPoolEntryInfo> rows;
    rows.reserve(dex.stringIds.size() + dex.typeIds.size() + dex.protoIds.size() +
                 dex.fieldIds.size() + dex.methodIds.size());
    const auto push = [&](ca::usize index, const char* tag, std::string text) {
        ConstantPoolEntryInfo row;
        row.index = index;
        row.tag   = Utf8String::from_cstr(tag);
        if (!text.empty()) row.text = utf8(text);
        rows.push_back(std::move(row));
    };
    for (ca::u32 index = 0; index < dex.stringIds.size(); ++index) {
        push(index, "StringId", textOf(dex.string(index)));
    }
    for (ca::u32 index = 0; index < dex.typeIds.size(); ++index) {
        push(index, "TypeId", textOf(dex.typeDescriptor(index)));
    }
    for (ca::u32 index = 0; index < dex.protoIds.size(); ++index) {
        if (!resolve) {
            push(index, "ProtoId", {});
            continue;
        }
        // proto 引用类条目：解析为 "(参数)返回" 文本。
        std::string text = "(";
        auto parameters = dex.protoParameters(index);
        if (parameters.is_ok()) {
            const TypeListSpan& span = parameters.unwrap();
            for (ca::u32 i = 0; i < span.size; ++i) {
                text += textOf(dex.typeDescriptor(span.at(i)));
            }
        }
        text += ")";
        text += textOf(dex.typeDescriptor(dex.protoIds[index].returnTypeIdx));
        push(index, "ProtoId", std::move(text));
    }
    for (ca::u32 index = 0; index < dex.fieldIds.size(); ++index) {
        if (!resolve) {
            push(index, "FieldId", {});
            continue;
        }
        const FieldKey key = fieldKeyOf(dex, index);
        push(index, "FieldId",
             key.valid() ? key.owner + "." + key.name + ":" + key.descriptor : std::string());
    }
    for (ca::u32 index = 0; index < dex.methodIds.size(); ++index) {
        if (!resolve) {
            push(index, "MethodId", {});
            continue;
        }
        const MethodKey key = methodKeyOf(dex, index);
        push(index, "MethodId",
             key.valid() ? key.owner + "." + key.name + key.descriptor : std::string());
    }
    return ca::core::Ok(std::move(rows));
}

//-- 引用面 --------------------------------------------------------------------

bool DexDomain::hasDynamicReferenceTables() const noexcept
{
    return std::any_of(dexes_.begin(), dexes_.end(), [](const auto& dex) {
        return dex && (!dex->callSites.empty() || !dex->methodHandles.empty());
    });
}

ReferenceScan DexDomain::buildReferenceScan() const
{
    RefIndex index;
    for (const auto& name : classOrder_) {
        const auto& slot = byName_.at(name);
        scanClassRefs(*dexes_[slot.dexIndex], slot.classDefIndex, name, index);
    }
    return ReferenceScan::fromParts(std::move(index.methods), std::move(index.fields),
                                    std::move(index.types), std::move(index.dynamicCalls));
}

WorkspaceResult<std::vector<ReferenceInfo>> DexDomain::xrefsToMethod(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(domainError(WorkspaceError::InvalidArgument, "xref method",
                                         "owner, name and descriptor are required"));
    }
    const auto scan = buildReferenceScan();
    return ca::core::Ok(collectReferenceInfos(
        scan,
        [&](const MethodReference& reference) {
            return reference.target.owner.to_std_string() == owner &&
                   reference.target.name.to_std_string() == name &&
                   reference.target.descriptor.to_std_string() == descriptor;
        },
        [](const FieldReference&) { return false; },
        [](const TypeReference&) { return false; }));
}

WorkspaceResult<std::vector<ReferenceInfo>> DexDomain::xrefsToField(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(domainError(WorkspaceError::InvalidArgument, "xref field",
                                         "owner, name and descriptor are required"));
    }
    const auto scan = buildReferenceScan();
    return ca::core::Ok(collectReferenceInfos(
        scan,
        [](const MethodReference&) { return false; },
        [&](const FieldReference& reference) {
            return reference.target.owner.to_std_string() == owner &&
                   reference.target.name.to_std_string() == name &&
                   reference.target.descriptor.to_std_string() == descriptor;
        },
        [](const TypeReference&) { return false; }));
}

WorkspaceResult<std::vector<ReferenceInfo>> DexDomain::xrefsToClass(
    const std::string& internalName) const
{
    if (internalName.empty()) {
        return ca::core::Err(domainError(WorkspaceError::InvalidArgument, "xref class",
                                         "internal name is required"));
    }
    const auto scan = buildReferenceScan();
    return ca::core::Ok(collectReferenceInfos(
        scan,
        [](const MethodReference&) { return false; },
        [](const FieldReference&) { return false; },
        [&](const TypeReference& reference) {
            return reference.targetType.to_std_string() == internalName;
        }));
}

WorkspaceResult<std::vector<ReferenceInfo>> DexDomain::calleesOfMethod(
    const std::string& owner, const std::string& name, const std::string& descriptor,
    bool resolved) const
{
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(domainError(WorkspaceError::InvalidArgument, "method callees",
                                         "owner, name and descriptor are required"));
    }
    const Slot* slot = findClass(owner);
    if (slot == nullptr) return ca::core::Err(notFound("method callees", owner));
    DexFile& dex = *dexes_[slot->dexIndex];
    auto data = dex.classData(slot->classDefIndex);
    if (data.is_err() || data.unwrap() == nullptr) {
        return ca::core::Err(notFound("method callees", owner + "." + name + descriptor));
    }
    const ClassData* classData = data.unwrap();
    const EncodedMethod* source = nullptr;
    for (const auto* group :
         {&classData->directMethods, &classData->virtualMethods}) {
        for (const EncodedMethod& method : *group) {
            const MethodKey key = methodKeyOf(dex, method.methodIndex);
            if (key.name == name && key.descriptor == descriptor) source = &method;
        }
    }
    if (source == nullptr) {
        return ca::core::Err(notFound("method callees", owner + "." + name + descriptor));
    }

    const ReferenceSite site{utf8(owner), utf8(name), utf8(descriptor), -1};
    std::vector<ReferenceInfo> result;
    std::set<std::string> seen;
    const DecodedBody body = decodeMethodBody(dex, *source);
    for (ca::u32 index = 0; index < body.insns.size(); ++index) {
        const Insn& insn = body.insns[index];
        if (insn.indexKind != IndexKind::Method && insn.indexKind != IndexKind::CallSite)
            continue;
        const ReferenceSite at{site.sourceClass.clone(), site.sourceMethod.clone(),
                               site.sourceDescriptor.clone(), static_cast<ca::i32>(index)};
        if (insn.indexKind == IndexKind::CallSite) {
            const auto* callSite = callSiteOf(dex, insn.index);
            if (callSite == nullptr) continue;
            const auto& values = callSite->values;
            const auto target = callSiteTargetText(dex, *callSite);
            if (!target.empty()) result.push_back(referenceInfo(at, target, "dynamic"));
            const auto bootstrap = methodKeyOf(dex, dex.methodHandles[values[0].index].targetIdx);
            if (bootstrap.valid())
                result.push_back(referenceInfo(at, methodTargetText(bootstrap), "bootstrap"));
            continue;
        }
        const MethodKey target = methodKeyOf(dex, insn.index);
        if (!target.valid()) continue;
        const std::string targetText = methodTargetText(target);
        if (seen.insert(targetText).second) {
            result.push_back(referenceInfo(at, targetText, "direct"));
        }
        if (!resolved) continue;
        // resolved：虚/接口调用（0x6e/0x72 及 range 变体 0x74/0x78）按 dex 域
        // 层次做 CHA 展开；static/super/direct 保持直接目标。域外类型不可解析，
        // 保持直接目标。
        const bool expandable = insn.opcode == 0x6e /* invoke-virtual */ ||
                                insn.opcode == 0x72 /* invoke-interface */ ||
                                insn.opcode == kOpInvokeVirtualR ||
                                insn.opcode == kOpInvokeInterfaceR;
        if (!expandable || !containsClass(target.owner)) continue;
        for (const std::string& candidate : classOrder_) {
            if (!isSubclassInDomain(candidate, target.owner)) continue;
            const Slot& candidateSlot = byName_.at(candidate);
            DexFile& candidateDex = *dexes_[candidateSlot.dexIndex];
            auto candidateData = candidateDex.classData(candidateSlot.classDefIndex);
            if (candidateData.is_err() || candidateData.unwrap() == nullptr) continue;
            const ClassData* candidateClassData = candidateData.unwrap();
            for (const auto* group :
                     {&candidateClassData->directMethods, &candidateClassData->virtualMethods}) {
                for (const EncodedMethod& method : *group) {
                    const MethodKey key = methodKeyOf(candidateDex, method.methodIndex);
                    if (key.name != target.name || key.descriptor != target.descriptor) continue;
                    // abstract 声明不是实现（CHA 收口口径）。
                    if ((method.accessFlags & 0x0400u) != 0) continue;
                    const std::string expanded = methodTargetText(key);
                    if (seen.insert(expanded).second) {
                        result.push_back(referenceInfo(at, expanded, "direct"));
                    }
                }
            }
        }
    }
    sortReferences(result);
    return ca::core::Ok(std::move(result));
}

//-- 层次面 --------------------------------------------------------------------

WorkspaceResult<std::vector<MethodOverrideInfo>> DexDomain::overridesOfMethod(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    std::vector<MethodOverrideInfo> out;
    const Slot* slot = findClass(owner);
    if (slot == nullptr) return ca::core::Err(notFound("method overrides", owner));

    // super 链声明收集：owner 自身（primary）+ 各祖先的同名同描述符声明。
    std::set<std::string> visited;
    std::string current = owner;
    bool first = true;
    while (!current.empty() && containsClass(current) && visited.insert(current).second) {
        const Slot& currentSlot = byName_.at(current);
        DexFile& dex = *dexes_[currentSlot.dexIndex];
        auto data = dex.classData(currentSlot.classDefIndex);
        if (data.is_ok() && data.unwrap() != nullptr) {
            const ClassData* classData = data.unwrap();
            bool declares = false;
            for (const auto* group :
         {&classData->directMethods, &classData->virtualMethods}) {
                for (const EncodedMethod& method : *group) {
                    const MethodKey key = methodKeyOf(dex, method.methodIndex);
                    if (key.name == name && key.descriptor == descriptor) declares = true;
                }
            }
            if (declares) {
                MethodOverrideInfo row;
                row.owner    = utf8(current);
                row.name     = utf8(name);
                row.descriptor = utf8(descriptor);
                row.primary  = first;
                out.push_back(std::move(row));
            }
        }
        first = false;
        const std::string super = internalNameOfDescriptor(
            textOf(dex.superclassDescriptor(currentSlot.classDefIndex)));
        current = super;
    }
    return ca::core::Ok(std::move(out));
}

//-- 常量事实面 ------------------------------------------------------------------

void DexDomain::visitStringTableValues(
    const std::function<void(std::string_view)>& visit) const
{
    for (const auto& dex : dexes_) {
        for (ca::u32 index = 0; index < dex->stringCount(); ++index) {
            const auto raw = dex->string(index);
            if (raw.is_err()) continue;
            const auto bytes = raw.unwrap();
            const auto value = mjbase::jasm::classFileUtf8ToString(
                std::string_view(bytes.data(), bytes.size()));
            visit(value);
        }
    }
}

WorkspaceResult<std::vector<DbStringRecord>> DexDomain::cpStringRecords(
    const std::string& internalName) const
{
    const Slot* slot = findClass(internalName);
    if (slot == nullptr) return ca::core::Err(notFound("constant strings", internalName));
    DexFile& dex = *dexes_[slot->dexIndex];

    // 字符串字面量（String 行）：const-string + 静态字符串初值，按 string_ids
    // 序去重（引擎 String 背书 utf8 去重口径）。
    std::set<ca::u32> literalIndexes;
    auto data = dex.classData(slot->classDefIndex);
    const ClassData* classData = data.is_ok() ? data.unwrap() : nullptr;
    if (classData != nullptr) {
        for (const auto* group :
         {&classData->directMethods, &classData->virtualMethods}) {
            for (const EncodedMethod& method : *group) {
                const DecodedBody body = decodeMethodBody(dex, method);
                for (const Insn& insn : body.insns) {
                    if (insn.indexKind == IndexKind::String) literalIndexes.insert(insn.index);
                }
            }
        }
        auto values = dex.staticValues(slot->classDefIndex);
        if (values.is_ok() && values.unwrap() != nullptr) {
            for (const EncodedValue& value : values.unwrap()->values) {
                if (value.type == EncodedValueType::String) literalIndexes.insert(value.index);
            }
        }
    }

    std::vector<DbStringRecord> records;
    std::set<std::string> seen;
    const auto pushString = [&](DbStringKind kind, ConstantUtf8Facet facet, const std::string& value) {
        const auto decoded = mjbase::jasm::classFileUtf8ToString(value);
        if (decoded.empty() || !seen.insert(decoded).second) return;
        DbStringRecord record;
        record.kind  = kind;
        record.facet = dbUtf8Facet(facet);
        record.value = utf8(decoded);
        records.push_back(std::move(record));
    };
    for (ca::u32 index : literalIndexes) {
        pushString(DbStringKind::String, ConstantUtf8Facet::Name, textOf(dex.string(index)));
    }

    // 名字/描述符引用集（裸 Utf8 行）：自身/成员身份语法串，facet 预分。
    pushString(DbStringKind::Utf8, constantUtf8Facet(internalName), internalName);
    const std::string super = internalNameOfDescriptor(
        textOf(dex.superclassDescriptor(slot->classDefIndex)));
    if (!super.empty()) pushString(DbStringKind::Utf8, constantUtf8Facet(super), super);
    {
        auto interfaces = dex.interfaces(slot->classDefIndex);
        if (interfaces.is_ok()) {
            const TypeListSpan& span = interfaces.unwrap();
            for (ca::u32 i = 0; i < span.size; ++i) {
                const std::string name =
                    internalNameOfDescriptor(textOf(dex.typeDescriptor(span.at(i))));
                pushString(DbStringKind::Utf8, constantUtf8Facet(name), name);
            }
        }
    }
    if (classData != nullptr) {
        for (const auto* group :
             {&classData->staticFields, &classData->instanceFields}) {
            for (const EncodedField& field : *group) {
                const FieldKey key = fieldKeyOf(dex, field.fieldIndex);
                if (!key.valid()) continue;
                pushString(DbStringKind::Utf8, constantUtf8Facet(key.name), key.name);
                pushString(DbStringKind::Utf8, constantUtf8Facet(key.descriptor),
                           key.descriptor);
            }
        }
        for (const auto* group :
         {&classData->directMethods, &classData->virtualMethods}) {
            for (const EncodedMethod& method : *group) {
                const MethodKey key = methodKeyOf(dex, method.methodIndex);
                if (!key.valid()) continue;
                pushString(DbStringKind::Utf8, constantUtf8Facet(key.name), key.name);
                pushString(DbStringKind::Utf8, constantUtf8Facet(key.descriptor),
                           key.descriptor);
            }
        }
    }
    return ca::core::Ok(std::move(records));
}

WorkspaceResult<std::vector<DbNumberRecord>> DexDomain::cpNumberRecords(
    const std::string& internalName) const
{
    const Slot* slot = findClass(internalName);
    if (slot == nullptr) return ca::core::Err(notFound("constant numbers", internalName));
    return ca::core::Ok(
        staticValueNumberRecords(*dexes_[slot->dexIndex], slot->classDefIndex));
}

WorkspaceResult<std::vector<MethodNumberSlice>> DexDomain::methodNumberSlices(
    const std::string& internalName) const
{
    const Slot* slot = findClass(internalName);
    if (slot == nullptr) return ca::core::Err(notFound("method number slices", internalName));
    DexFile& dex = *dexes_[slot->dexIndex];
    std::vector<MethodNumberSlice> slices;
    auto data = dex.classData(slot->classDefIndex);
    if (data.is_err() || data.unwrap() == nullptr) return ca::core::Ok(std::move(slices));
    const ClassData* classData = data.unwrap();
    for (const auto* group :
         {&classData->directMethods, &classData->virtualMethods}) {
        for (const EncodedMethod& method : *group) {
            MethodNumberSlice slice;
            const MethodKey key = methodKeyOf(dex, method.methodIndex);
            slice.name       = key.name;
            slice.descriptor = key.descriptor;
            const DecodedBody body = decodeMethodBody(dex, method);
            slice.records.reserve(body.insns.size());
            for (ca::u32 index = 0; index < body.insns.size(); ++index) {
                const Insn& insn = body.insns[index];
                if (insn.opcode < kOpConst4 || insn.opcode > kOpConstWideHigh16) continue;
                const bool wide = insn.opcode >= kOpConstWide16;
                slice.records.push_back(dbIntNumberRecord(
                    static_cast<ca::i64>(index), static_cast<ca::i32>(insn.opcode),
                    wide ? DbNumberKind::Long : DbNumberKind::Int, insn.literal));
            }
            slices.push_back(std::move(slice));
        }
    }
    return ca::core::Ok(std::move(slices));
}

//-- 文本面 ----------------------------------------------------------------------

WorkspaceResult<std::string> DexDomain::smaliTextAt(const Slot& slot) const
{
    auto text = dex::smali::disassembleClass(*dexes_[slot.dexIndex], slot.classDefIndex);
    if (text.is_err()) {
        return ca::core::Err(domainError(WorkspaceError::DataLoss, "smali text",
                                         to_string(text.unwrap_err())));
    }
    return ca::core::Ok(std::move(text).unwrap());
}

WorkspaceResult<std::string> DexDomain::smaliText(const std::string& uriOrInternalName) const
{
    const Slot* slot = findClass(uriOrInternalName);
    if (slot == nullptr) return ca::core::Err(notFound("smali text", uriOrInternalName));
    return smaliTextAt(*slot);
}

WorkspaceResult<std::vector<SearchHit>> DexDomain::searchInstructions(
    const std::string& query, const SearchOptions& options) const
{
    // 参数校验 + regex 预编译（search_match 单源；空 query/非法 regex 的
    // 错误面与引擎逐字节同形）。
    auto compiled = compileSearchRegex(query, options, "search instructions");
    if (compiled.is_err()) return ca::core::Err(std::move(compiled).unwrap_err());
    const std::optional<std::regex> pattern = std::move(compiled).unwrap();
    const std::regex* patternPtr = pattern.has_value() ? &*pattern : nullptr;

    std::vector<SearchHit> hits;
    for (const auto& owner : classOrder_) {
        if (!matchesSearchScope(owner, options)) continue;
        const auto& slot = byName_.at(owner);
        DexFile& dex = *dexes_[slot.dexIndex];
        auto data = dex.classData(slot.classDefIndex);
        if (data.is_err() || data.unwrap() == nullptr) continue;
        const ClassData* classData = data.unwrap();
        for (const auto* group : {&classData->directMethods, &classData->virtualMethods}) {
            for (const EncodedMethod& method : *group) {
                const MethodKey key = methodKeyOf(dex, method.methodIndex);
                if (!key.valid()) continue;
                const DecodedBody body = decodeMethodBody(dex, method);
                for (ca::u32 index = 0; index < body.insns.size(); ++index) {
                    const Insn& insn = body.insns[index];
                    if (options.opcode >= 0 &&
                        static_cast<ca::i32>(insn.opcode) != options.opcode) {
                        continue;
                    }
                    const std::string line = renderInsnLine(dex, insn);
                    if (!matchesText(line, query, options, patternPtr)) continue;
                    SearchHit hit;
                    hit.resourceUri = utf8(dexUris_[slot.dexIndex]);
                    hit.kind        = Utf8String::from_cstr("instruction");
                    hit.owner       = utf8(owner);
                    hit.name        = utf8(key.name);
                    hit.descriptor  = utf8(key.descriptor);
                    hit.instructionIndex = static_cast<ca::i32>(index);
                    hit.value       = utf8(line);
                    hits.push_back(std::move(hit));
                }
            }
        }
    }
    return ca::core::Ok(std::move(hits));
}

//-- 输入嗅探 ----------------------------------------------------------------------

DexArtifactForm probeDexArtifact(const std::string& path)
{
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input.is_open()) return DexArtifactForm::None;
    ca::u8 head[4] = {0, 0, 0, 0};
    input.read(reinterpret_cast<char*>(head), sizeof(head));
    if (input.gcount() < 4) return DexArtifactForm::None;
    if (head[0] == 'd' && head[1] == 'e' && head[2] == 'x' && head[3] == '\n') {
        return DexArtifactForm::Dex;
    }
    if (head[0] == 'P' && head[1] == 'K' && head[2] == 0x03 && head[3] == 0x04) {
        // zip 族：扩展名 .apk（大小写不敏感）判 apk。
        std::string extension = std::filesystem::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".apk") return DexArtifactForm::Apk;
    }
    return DexArtifactForm::None;
}

const char* to_string(DexArtifactForm form) noexcept
{
    switch (form) {
    case DexArtifactForm::Apk: return "apk";
    case DexArtifactForm::Dex: return "dex";
    case DexArtifactForm::None: break;
    }
    return "none";
}

}   // namespace mjt::workspace
