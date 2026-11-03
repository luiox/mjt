#include "classfile_support.hpp"

#include <cstring>
#include <sstream>
#include <utility>

namespace mjt::workspace::support {

namespace {

using jbc::core::AnnotationsAttr;
using jbc::core::ConstantPool;
using jbc::core::CpTag;

/// @brief std::string → 拥有型 Utf8String（DTO 文本字段赋值口径，与
///        analysis_ingest 的 utf8() 同款：cp modified UTF-8 字节保真，不二次校验）。
ca::str::Utf8String utf8(const std::string& value)
{
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

/// @brief u2 大端读取（Raw 载荷解析）。
ca::u32 readU2(const std::vector<ca::u8>& bytes, ca::usize at)
{
    return (static_cast<ca::u32>(bytes[at]) << 8) | bytes[at + 1];
}

}   // namespace

std::string renderFloatBits(ca::u32 bits)
{
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    std::ostringstream out;
    out << value;
    return out.str();
}

std::string renderDoubleBits(ca::u64 bits)
{
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    std::ostringstream out;
    out << value;
    return out.str();
}

std::string annotationValueText(const jbc::core::ClassFile& cf,
                                const AnnotationsAttr::ElemValue& value)
{
    const ConstantPool::Entry* entry =
        cf.cp.valid(value.cp) ? &cf.cp.at(value.cp) : nullptr;
    switch (value.tag) {
    case 's':
        // String 条目：a 指向 Utf8。
        if (entry != nullptr && entry->tag == CpTag::String) return utf8Text(cf, entry->a);
        return {};
    case 'e':
        return utf8Text(cf, value.typeNameCp) + "." + utf8Text(cf, value.constNameCp);
    case 'c':
        return utf8Text(cf, value.cp);
    case 'Z':
        return entry != nullptr && entry->ival != 0 ? "true" : "false";
    case 'B':
    case 'C':
    case 'S':
    case 'I':
        return entry != nullptr ? std::to_string(entry->ival) : std::string("0");
    case 'J':
        return entry != nullptr ? std::to_string(entry->lval) : std::string("0");
    case 'F':
        return entry != nullptr ? renderFloatBits(static_cast<ca::u32>(entry->ival))
                                : std::string("0");
    case 'D':
        return entry != nullptr ? renderDoubleBits(static_cast<ca::u64>(entry->lval))
                                : std::string("0");
    case '@':
        return value.nested != nullptr ? utf8Text(cf, value.nested->typeCp) : std::string();
    default:
        return {};
    }
}

void collectAnnotationElement(const jbc::core::ClassFile& cf, std::string_view name,
                              const AnnotationsAttr::ElemValue& value, AnnotationElement& out)
{
    out.name  = utf8(std::string(name));
    out.tag   = static_cast<char>(value.tag);
    out.value = utf8(annotationValueText(cf, value));
    if (value.tag == '@' && value.nested != nullptr) {
        out.annotationDescriptor = utf8(utf8Text(cf, value.nested->typeCp));
        for (const auto& [nameCp, nested] : value.nested->values) {
            AnnotationElement child;
            collectAnnotationElement(cf, utf8Of(cf, nameCp), nested, child);
            out.children.push_back(std::move(child));
        }
    }
    else if (value.tag == '[') {
        for (const AnnotationsAttr::ElemValue& item : value.items) {
            AnnotationElement child;
            collectAnnotationElement(cf, "", item, child);
            out.children.push_back(std::move(child));
        }
    }
}

void collectAnnotations(jbc::core::ClassFile& cf, std::vector<jbc::core::AttrSlot>& attrs,
                        std::vector<AnnotationInfo>& out)
{
    for (jbc::core::AttrSlot& slot : attrs) {
        const bool visible = cf.cp.utf8Equals(slot.nameCp, "RuntimeVisibleAnnotations");
        if (!visible && !cf.cp.utf8Equals(slot.nameCp, "RuntimeInvisibleAnnotations")) continue;
        if (!slot.decoded()) {
            if (cf.decodeAttr(slot).is_err()) continue;
        }
        const auto* body = ca::core::tag_cast::cast<AnnotationsAttr>(slot.peek());
        if (body == nullptr) continue;
        for (const AnnotationsAttr::Annotation& anno : body->annos) {
            AnnotationInfo info;
            info.descriptor = utf8(utf8Text(cf, anno.typeCp));
            info.visible    = visible;
            for (const auto& [nameCp, elem] : anno.values) {
                AnnotationElement element;
                collectAnnotationElement(cf, utf8Of(cf, nameCp), elem, element);
                info.elements.push_back(std::move(element));
            }
            out.push_back(std::move(info));
        }
    }
}

void collectLocalVariables(jbc::core::ClassFile& cf, jbc::core::CodeAttr& code,
                           std::vector<LocalVariableInfo>& out)
{
    for (jbc::core::AttrSlot& slot : code.attrs) {
        const bool typeTable = cf.cp.utf8Equals(slot.nameCp, "LocalVariableTypeTable");
        if (!typeTable && !cf.cp.utf8Equals(slot.nameCp, "LocalVariableTable")) continue;
        if (!slot.decoded()) {
            if (cf.decodeAttr(slot).is_err()) continue;
        }
        const auto* body = ca::core::tag_cast::cast<jbc::core::LocalVariableTableAttr>(slot.peek());
        if (body == nullptr) continue;
        for (const jbc::core::LocalVariableTableAttr::Row& row : body->rows) {
            LocalVariableInfo info;
            info.name       = utf8(utf8Text(cf, row.nameCp));
            info.descriptor = utf8(utf8Text(cf, row.descCp));
            info.index      = static_cast<ca::i32>(row.slot);
            out.push_back(std::move(info));
        }
    }
}

std::optional<ConstantValueInfo> fieldConstantValue(jbc::core::ClassFile& cf,
                                                    jbc::core::MemberInfo& field)
{
    const jbc::core::AttrSlot* slot = findAttr(cf, field.attrs, "ConstantValue");
    if (slot == nullptr) return std::nullopt;
    if (!slot->decoded()) {
        if (cf.decodeAttr(const_cast<jbc::core::AttrSlot&>(*slot)).is_err()) return std::nullopt;
    }
    // 0.4.16+ typed 面：解码命中 ConstantValueAttr（constantValueCp 单域）；
    // 拒收形态落 Raw 槽 2 字节原读。cast 无检查，kind 分道先于 cast。
    ca::u32 cpIndex = 0;
    if (slot->peek()->kind() == jbc::core::AttrKind::ConstantValue) {
        cpIndex = ca::core::tag_cast::cast<const jbc::core::ConstantValueAttr*>(slot->peek())
                      ->constantValueCp;
    }
    else {
        const auto* raw = ca::core::tag_cast::cast<const jbc::core::RawAttr>(slot->peek());
        if (raw == nullptr || raw->bytes().size() < 2) return std::nullopt;
        cpIndex = readU2(raw->bytes(), 0);
    }
    if (!cf.cp.valid(cpIndex)) return std::nullopt;
    const ConstantPool::Entry& entry = cf.cp.at(cpIndex);
    ConstantValueInfo info;
    switch (entry.tag) {
    case CpTag::Integer:
        info.kind = ConstantValueInfo::Kind::Integer;
        info.text = utf8(std::to_string(entry.ival));
        break;
    case CpTag::Long:
        info.kind = ConstantValueInfo::Kind::Long;
        info.text = utf8(std::to_string(entry.lval));
        break;
    case CpTag::Float:
        info.kind = ConstantValueInfo::Kind::Float;
        info.text = utf8(renderFloatBits(static_cast<ca::u32>(entry.ival)));
        break;
    case CpTag::Double:
        info.kind = ConstantValueInfo::Kind::Double;
        info.text = utf8(renderDoubleBits(static_cast<ca::u64>(entry.lval)));
        break;
    case CpTag::String:
        info.kind = ConstantValueInfo::Kind::String;
        info.text = utf8(utf8Text(cf, entry.a));
        break;
    default:
        return std::nullopt;
    }
    return info;
}

ConstantPoolEntryInfo constantPoolEntry(const jbc::core::ClassFile& cf, ca::u32 index, bool resolve)
{
    ConstantPoolEntryInfo info;
    info.index = index;
    if (!cf.cp.valid(index)) {
        info.tag       = ca::str::Utf8String{};
        info.text      = ca::str::Utf8String{};
        info.malformed = true;
        return info;
    }
    const ConstantPool::Entry& entry = cf.cp.at(index);
    switch (entry.tag) {
    case CpTag::Utf8:
        info.tag  = ca::str::Utf8String::from_cstr("Utf8");
        info.text = utf8(utf8Text(cf, index));
        break;
    case CpTag::Integer:
        info.tag  = ca::str::Utf8String::from_cstr("Integer");
        info.text = utf8(std::to_string(entry.ival));
        break;
    case CpTag::Float:
        info.tag  = ca::str::Utf8String::from_cstr("Float");
        info.text = utf8(renderFloatBits(static_cast<ca::u32>(entry.ival)));
        break;
    case CpTag::Long:
        info.tag  = ca::str::Utf8String::from_cstr("Long");
        info.text = utf8(std::to_string(entry.lval));
        break;
    case CpTag::Double:
        info.tag  = ca::str::Utf8String::from_cstr("Double");
        info.text = utf8(renderDoubleBits(static_cast<ca::u64>(entry.lval)));
        break;
    case CpTag::Class:
        info.tag  = ca::str::Utf8String::from_cstr("Class");
        info.text = utf8(utf8Text(cf, entry.a));
        break;
    case CpTag::String:
        info.tag  = ca::str::Utf8String::from_cstr("String");
        info.text = utf8(utf8Text(cf, entry.a));
        break;
    case CpTag::Fieldref:
    case CpTag::Methodref:
    case CpTag::InterfaceMethodref: {
        info.tag = entry.tag == CpTag::Fieldref
                       ? ca::str::Utf8String::from_cstr("Fieldref")
                   : entry.tag == CpTag::Methodref
                       ? ca::str::Utf8String::from_cstr("Methodref")
                       : ca::str::Utf8String::from_cstr("InterfaceMethodref");
        if (!resolve) break;
        // 引用条目：class_index → Class，name_and_type_index → name + desc。
        const std::string owner = classNameOf(cf, entry.a);
        std::string name;
        std::string desc;
        if (cf.cp.valid(entry.b)) {
            const ConstantPool::Entry& nt = cf.cp.at(entry.b);
            if (nt.tag == CpTag::NameAndType) {
                name = utf8Text(cf, nt.a);
                desc = utf8Text(cf, nt.b);
            }
        }
        info.text = utf8(owner + "." + name + ":" + desc);
        break;
    }
    case CpTag::NameAndType:
        info.tag = ca::str::Utf8String::from_cstr("NameAndType");
        // 引用类条目与 workspace_types.hpp 的 resolve 契约对齐：仅 resolve 时给文本。
        if (resolve) info.text = utf8(utf8Text(cf, entry.a) + ":" + utf8Text(cf, entry.b));
        break;
    case CpTag::MethodHandle:
        info.tag = ca::str::Utf8String::from_cstr("MethodHandle");
        // a = reference_kind，b = 引用条目；文本仅 resolve 时给。
        if (resolve) {
            info.text = utf8("kind " + std::to_string(entry.a) + " -> " +
                        constantPoolEntry(cf, entry.b, true).text.to_std_string());
        }
        break;
    case CpTag::MethodType:
        info.tag  = ca::str::Utf8String::from_cstr("MethodType");
        info.text = utf8(utf8Text(cf, entry.a));
        break;
    case CpTag::Dynamic:
    case CpTag::InvokeDynamic:
        info.tag = entry.tag == CpTag::Dynamic ? ca::str::Utf8String::from_cstr("Dynamic")
                                                : ca::str::Utf8String::from_cstr("InvokeDynamic");
        if (resolve) {
            std::string nameAndDesc;
            if (cf.cp.valid(entry.b)) {
                const ConstantPool::Entry& nt = cf.cp.at(entry.b);
                if (nt.tag == CpTag::NameAndType)
                    nameAndDesc = utf8Text(cf, nt.a) + ":" + utf8Text(cf, nt.b);
            }
            info.text = utf8(nameAndDesc + " @bsm " + std::to_string(entry.a));
        }
        break;
    case CpTag::Module:
        info.tag  = ca::str::Utf8String::from_cstr("Module");
        info.text = utf8(utf8Text(cf, entry.a));
        break;
    case CpTag::Package:
        info.tag  = ca::str::Utf8String::from_cstr("Package");
        info.text = utf8(utf8Text(cf, entry.a));
        break;
    }
    return info;
}

std::string objectClassFromArrayDescriptor(std::string_view descriptor)
{
    // 剥掉全部 '[' 后必须是 'L'+name+';' 才有对象类；基本类型数组返回空。
    ca::usize at = 0;
    while (at < descriptor.size() && descriptor[at] == '[') ++at;
    if (at == 0 || at >= descriptor.size()) return {};
    return objectClassFromFieldDescriptor(descriptor.substr(at));
}

}   // namespace mjt::workspace::support
