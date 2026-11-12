/// @file        analysis_ops.cpp
/// @brief       查询算子实现：从 WorkspaceBase 成员整体迁入（W3），零语义
///              变化——容器 + 选择器进，DTO 出；LVT 显示名策略位由调用方
///              传入（判定函数 lvtJarFallbackOf 亦在此，供其缓存）。
#include "analysis_ops.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string_view>

#include "jbc/core/attr_codec.hpp"

#include "mjbase/analysis/lvt_display_policy.hpp"
#include "mjbase/jasm/classfile_utf8.hpp"
#include "mjbase/spi/class_file_sanitize.hpp"

#include "classfile_support.hpp"
#include "edit_support.hpp"
#include "jar_ops.hpp"
#include "summary_scan.hpp"

namespace mjt::workspace {
namespace {

using support::utf8Text;

/// @brief 资源类型短名（小写口径："class"/"manifest"/"jar"/"resource"）。
std::string resourceTypeName(model::ResourceType type)
{
    switch (type) {
    case model::ResourceType::Class: return "class";
    case model::ResourceType::Manifest: return "manifest";
    case model::ResourceType::Jar: return "jar";
    default: return "resource";
    }
}

/// @brief Code 嵌套 LNT 行 → {line, 代码指令序号}；pc 无对应指令时 index 为 -1。
WorkspaceResult<std::vector<LineNumberInfo>> lineNumbersOf(jbc::ir::ClassSession& session,
                                                           jbc::core::MemberInfo& method,
                                                           ca::usize              methodIndex)
{
    std::vector<LineNumberInfo> result;
    jbc::core::CodeAttr*        code = support::ensureCode(session.backing(), method);
    if (code == nullptr) return ca::core::Ok(std::move(result));

    std::vector<std::pair<ca::u32, ca::i32>> rows;   // {pc, line}
    for (jbc::core::AttrSlot& nested : code->attrs) {
        if (!session.backing().cp.utf8Equals(nested.nameCp, "LineNumberTable")) continue;
        if (!nested.decoded()) {
            if (session.backing().decodeAttr(nested).is_err()) continue;
        }
        const auto* table =
            ca::core::tag_cast::cast<const jbc::core::LineNumberTableAttr>(nested.peek());
        if (table == nullptr) continue;
        for (const auto& row : table->rows)
            rows.emplace_back(row.startPc, static_cast<ca::i32>(row.line));
    }
    if (rows.empty()) return ca::core::Ok(std::move(result));

    auto promoted = jbc::ir::promoteMethod(session, methodIndex);
    if (promoted.is_err())
        return ca::core::Err(
            error(WorkspaceError::DataLoss, "decode method", "method body cannot be promoted"));
    auto raw = promoted.unwrap()->dom.exportRaw();
    if (raw.is_err())
        return ca::core::Err(
            error(WorkspaceError::DataLoss, "decode method", "method body cannot be exported"));
    const auto insns = std::move(raw).unwrap();
    for (const auto& [pc, line] : rows) {
        LineNumberInfo info;
        info.line = line;
        for (ca::usize index = 0; index < insns.size(); ++index) {
            if (insns[index].pc == pc) {
                info.instructionIndex = static_cast<ca::i32>(index);
                break;
            }
        }
        result.push_back(info);
    }
    return ca::core::Ok(std::move(result));
}

/// @brief 把 LVT 显示名策略应用到查询输出的 locals（B3，#121 §4.4）。
///
/// 只改呈现名：行数、槽位、描述符原样——LVT 原文在字节码里照存，判定
/// 结果也不入库（v1 决策）。呈现核心在 mjbase::analysis（纯函数面）。
void applyLvtDisplayPolicy(std::vector<LocalVariableInfo>& locals, bool jarFallback)
{
    if (locals.empty()) return;
    std::vector<mjbase::analysis::LvtDisplayRow> rows;
    rows.reserve(locals.size());
    for (const auto& local : locals)
        rows.push_back({local.name.ref(), local.descriptor.ref(),
                        static_cast<ca::u32>(local.index)});
    const auto names = mjbase::analysis::lvtDisplayNames(rows, jarFallback);
    for (ca::usize index = 0; index < locals.size(); ++index)
        locals[index].name = names[index].clone();
}

WorkspaceResult<MethodInfo> makeMethodInfo(jbc::ir::ClassSession& session,
                                           jbc::core::MemberInfo& method, ca::usize methodIndex,
                                           bool lvtFallback)
{
    jbc::core::ClassFile& cf = session.backing();
    MethodInfo output;
    output.name       = utf8(utf8Text(cf, method.nameCp));
    output.descriptor = utf8(utf8Text(cf, method.descCp));
    output.signature  = utf8(support::signatureText(cf, method.attrs));
    output.access     = static_cast<ca::i32>(method.access);
    if (jbc::core::CodeAttr* code = support::ensureCode(cf, method)) {
        output.maxStack      = static_cast<ca::i32>(code->maxStack);
        output.maxLocals     = static_cast<ca::i32>(code->maxLocals);
        output.tryCatchCount = code->excTable.size();
        // instructionCount：代码指令序数（CodeDom 程序顺序；label/line 不是指令）。
        auto promoted = jbc::ir::promoteMethod(session, methodIndex);
        if (promoted.is_err())
            return ca::core::Err(error(
                WorkspaceError::DataLoss, "decode method",
                "method body cannot be promoted: " + utf8Text(cf, method.nameCp) + " (" +
                    std::to_string(static_cast<int>(promoted.unwrap_err())) + ")"));
        output.instructionCount = promoted.unwrap()->dom.size();
        support::collectLocalVariables(cf, *code, output.localVariables);
        applyLvtDisplayPolicy(output.localVariables, lvtFallback);
    }
    output.exceptions = utf8List(support::exceptionNames(cf, method));
    auto lines        = lineNumbersOf(session, method, methodIndex);
    if (lines.is_err()) return ca::core::Err(std::move(lines).unwrap_err());
    output.lineNumbers = std::move(lines).unwrap();
    support::collectAnnotations(cf, method.attrs, output.annotations);
    return ca::core::Ok(std::move(output));
}

/// @brief InnerClasses 属性的内部类名（去重排序；inner_name_index=0 的匿名形态跳过）。
///
/// typed InnerClassesAttr 体经公开解码入口读出（B3 分派接线前后同形，不触碰
/// 槽状态）；解码失败的槽跳过（查询面对畸形输入宽容）。
std::vector<std::string> innerClassNames(jbc::core::ClassFile& cf)
{
    std::vector<std::string> names;
    for (jbc::core::AttrSlot& slot : cf.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "InnerClasses")) continue;
        auto d = jbc::core::decodeInnerClassesBody(cf, slot);
        if (d.is_err()) continue;
        const auto body = std::move(d).unwrap();
        const auto& ic  = static_cast<const jbc::core::InnerClassesAttr&>(*body);
        for (const jbc::core::InnerClassesAttr::Row& row : ic.rows) {
            if (row.innerNameCp == 0) continue;
            const auto name = mjbase::jasm::classFileUtf8ToString(cf.utf8(row.innerNameCp));
            if (!name.empty()) names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

WorkspaceResult<ClassInfo> decodeClass(const std::string& uri, const model::ClassResource& resource,
                                       bool lvtFallback)
{
    auto session = sessionOf(resource.get(), "decode class " + uri);
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto                  held = std::move(session).unwrap();
    jbc::core::ClassFile& cf   = held->backing();

    ClassInfo output;
    output.resourceUri  = utf8(uri);
    // cp modified UTF-8 → 规范形 UTF-8：内名/父类名与容器 URI、参数面同形，
    // 增补平面类名（cp 内代理对形）不解码会与 UTF-8 形选择器比对 miss。
    output.internalName = utf8(mjbase::jasm::classFileUtf8ToString(cf.thisName()));
    output.superName    = utf8(mjbase::jasm::classFileUtf8ToString(cf.superName()));
    output.signature    = utf8(support::signatureText(cf, cf.attrs));
    output.access       = static_cast<ca::i32>(cf.access);
    // version = major | (minor << 16)。
    output.version      = static_cast<ca::i32>(cf.major) | (static_cast<ca::i32>(cf.minor) << 16);
    for (const auto interfaceCp : cf.interfaces) {
        output.interfaces.push_back(utf8(std::string{cf.className(interfaceCp)}));
    }
    for (auto& field : cf.fields) output.fields.push_back(makeFieldInfo(cf, field));
    for (ca::usize index = 0; index < cf.methods.size(); ++index) {
        auto info = makeMethodInfo(*held, cf.methods[index], index, lvtFallback);
        if (info.is_err()) return ca::core::Err(std::move(info).unwrap_err());
        output.methods.push_back(std::move(info).unwrap());
    }
    output.innerClasses = utf8List(innerClassNames(cf));
    support::collectAnnotations(cf, cf.attrs, output.annotations);
    return ca::core::Ok(std::move(output));
}

/// @brief jasm::Result 错误行组装（getJasm/getMethodJasm 共用）。
template <typename T>
jasm::Result<T> jasmFailure(const std::string& message)
{
    jasm::Result<T> result;
    result.diagnostics.push_back(
        {jasm::DiagnosticSeverity::Error, jasm::DiagnosticStage::Resource, utf16(message),
         std::nullopt});
    return result;
}

void hashFingerprintBytes(ca::u64& state, const ca::u8* data, ca::usize size)
{
    constexpr ca::u64 kPrime = 1099511628211ULL;
    for (ca::usize index = 0; index < size; ++index) {
        state ^= data[index];
        state *= kPrime;
    }
}

void hashFingerprintLength(ca::u64& state, ca::usize value)
{
    const auto stableValue = static_cast<ca::u64>(value);
    for (ca::usize index = 0; index < sizeof(stableValue); ++index) {
        const auto byte = static_cast<ca::u8>((stableValue >> (index * 8U)) & 0xffU);
        hashFingerprintBytes(state, &byte, 1);
    }
}

}   // namespace

WorkspaceResult<std::string> resolveClassUri(const model::ResourceContainer& container,
                                             const std::string&              uriOrInternalName)
{
    if (uriOrInternalName.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "resolve class", "selector is empty"));
    if (ca::core::tag_cast::isa<model::ClassResource>(container.getResource(uriOrInternalName)))
        return ca::core::Ok(uriOrInternalName);
    try {
        // 内名 selector 命中 Multi-Release 族（base 与 versions/<N> 变体并存，
        // 内名相同）时按确定序选定：base 形条目优先于版本化条目（JEP 238 口径
        // 版本 ≥ 9），同层级取 URI 字节序最小。不得依赖容器迭代序——那会让
        // 同一容器在不同进程/标准库里解析到不同族员。
        std::string match;
        ca::i64     matchVersion = -1;
        container.visitResources([&](std::string_view uri, const model::IResource& resource) {
            const auto* classResource =
                ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
            if (classResource == nullptr) return;
            if (internalNameOfBytes(classResource->get()) != uriOrInternalName) return;
            const ca::i64 version = model::multiReleaseVersionOfClassUri(uri);
            const bool    candidateVersioned = version >= 9;
            const bool    currentVersioned   = matchVersion >= 9;
            const bool    better = match.empty() ||
                                   (!candidateVersioned && currentVersioned) ||
                                   (candidateVersioned == currentVersioned &&
                                    std::string_view(uri) < std::string_view(match));
            if (!better) return;
            match        = std::string(uri);
            matchVersion = version;
        });
        if (match.empty())
            return ca::core::Err(
                error(WorkspaceError::NotFound, "resolve class", uriOrInternalName));
        return ca::core::Ok(std::move(match));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::DataLoss, "resolve class", exception.what()));
    }
}

WorkspaceResult<std::vector<ResourceSummary>> listResources(const model::ResourceContainer& container,
                                                            bool includeClasses)
{
    std::vector<ResourceSummary> result;
    container.visitResources([&](std::string_view uri, const model::IResource& resource) {
        if (!includeClasses && resource.getType() == model::ResourceType::Class) return;
        auto bytes = bytesOf(resource);
        if (bytes.is_err()) return;
        result.push_back({utf8(uri), utf8(resourceTypeName(resource.getType())),
                          bytes.unwrap().size()});
    });
    std::sort(result.begin(), result.end(),
              [](const ResourceSummary& left, const ResourceSummary& right) {
                  return left.uri < right.uri;
              });
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<ResourceContent> readResource(const model::ResourceContainer& container,
                                              ca::str::Utf8StringRef          uriRef)
{
    const std::string uri = uriRef.to_std_string();
    const model::IResource* resource = container.getResource(uri);
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "read resource", uri));
    auto bytes = bytesOf(*resource);
    if (bytes.is_err()) return ca::core::Err(std::move(bytes).unwrap_err());
    ResourceContent result;
    result.summary = {utf8(uri), utf8(resourceTypeName(resource->getType())), bytes.unwrap().size()};
    result.bytes   = std::move(bytes).unwrap();
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<ClassBytesInfo> readClassBytes(const model::ResourceContainer& container,
                                               ca::str::Utf8StringRef          uriOrInternalNameRef)
{
    const std::string uriOrInternalName = uriOrInternalNameRef.to_std_string();
    std::string       uri;
    if (hasClassExtension(uriOrInternalName) &&
        container.getResource(uriOrInternalName) != nullptr) {
        uri = uriOrInternalName;
    }
    else {
        auto resolved = resolveClassUri(container, uriOrInternalName);
        if (resolved.is_err()) return ca::core::Err(std::move(resolved).unwrap_err());
        uri = std::move(resolved).unwrap();
    }
    const model::IResource* resource = container.getResource(uri);
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "read class bytes", uri));
    auto bytes = bytesOf(*resource);
    if (bytes.is_err()) return ca::core::Err(std::move(bytes).unwrap_err());

    ClassBytesInfo result;
    result.resourceUri  = utf8(uri);
    result.bytes        = std::move(bytes).unwrap();
    result.fingerprint  = utf8(fingerprintBytes(result.bytes));
    result.internalName = utf8(internalNameOfBytes(result.bytes));
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<std::vector<ClassSummary>> listClassSummaries(
    const model::ResourceContainer& container)
{
    std::vector<ClassSummary> result;
    container.visitResources([&](std::string_view uri, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        auto decoded = decodeClassSummary(std::string(uri), *classResource);
        if (decoded.is_err()) return;
        result.push_back(std::move(decoded).unwrap());
    });
    std::sort(result.begin(), result.end(),
              [](const ClassSummary& left, const ClassSummary& right) {
                  if (left.internalName != right.internalName)
                      return left.internalName < right.internalName;
                  return left.resourceUri < right.resourceUri;
              });
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<std::vector<ClassInfo>> listClasses(const model::ResourceContainer& container,
                                                    bool lvtJarFallback)
{
    std::vector<ClassInfo> result;
    container.visitResources([&](std::string_view uri, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        auto decoded = decodeClass(std::string(uri), *classResource, lvtJarFallback);
        if (decoded.is_err()) return;
        result.push_back(std::move(decoded).unwrap());
    });
    std::sort(result.begin(), result.end(), [](const ClassInfo& left, const ClassInfo& right) {
        if (left.internalName != right.internalName) return left.internalName < right.internalName;
        return left.resourceUri < right.resourceUri;
    });
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<ClassInfo> classInfo(const model::ResourceContainer& container,
                                     ca::str::Utf8StringRef uriOrInternalNameRef,
                                     bool                   lvtJarFallback)
{
    const std::string uriOrInternalName = uriOrInternalNameRef.to_std_string();
    auto              uri               = resolveClassUri(container, uriOrInternalName);
    if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        container.getResource(uri.unwrap()));
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "get class info", uri.unwrap()));
    return decodeClass(uri.unwrap(), *resource, lvtJarFallback);
}

WorkspaceResult<MethodInfo> methodInfo(const model::ResourceContainer& container,
                                       ca::str::Utf8StringRef ownerRef, ca::str::Utf8StringRef nameRef,
                                       ca::str::Utf8StringRef descriptorRef, bool lvtJarFallback)
{
    const std::string name       = nameRef.to_std_string();
    const std::string descriptor = descriptorRef.to_std_string();
    if (name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "get method info",
                                   "name and descriptor are required"));
    }
    auto decoded = classInfo(container, ownerRef, lvtJarFallback);
    if (decoded.is_err()) return ca::core::Err(std::move(decoded).unwrap_err());
    // ClassInfo 换型 move-only：unwrap 走移动重载，命中行按移动语义返回。
    for (auto& method : std::move(decoded).unwrap().methods) {
        if (method.name == utf8Ref(name) && method.descriptor == utf8Ref(descriptor))
            return ca::core::Ok(std::move(method));
    }
    const std::string owner = ownerRef.to_std_string();
    return ca::core::Err(
        error(WorkspaceError::NotFound, "get method info", owner + "." + name + descriptor));
}

WorkspaceResult<MethodBytecode> methodBytecode(const model::ResourceContainer& container,
                                               ca::str::Utf8StringRef ownerRef,
                                               ca::str::Utf8StringRef nameRef,
                                               ca::str::Utf8StringRef descriptorRef,
                                               bool                   lvtJarFallback)
{
    const std::string owner      = ownerRef.to_std_string();
    const std::string name       = nameRef.to_std_string();
    const std::string descriptor = descriptorRef.to_std_string();
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "get method bytecode",
                                   "owner, name and descriptor are required"));
    }
    auto uri = resolveClassUri(container, owner);
    if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        container.getResource(uri.unwrap()));
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "get method bytecode", owner));
    auto session = sessionOf(resource->get(), "get method bytecode");
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto                  held = std::move(session).unwrap();
    jbc::core::ClassFile& cf   = held->backing();
    for (ca::usize methodIndex = 0; methodIndex < cf.methods.size(); ++methodIndex) {
        auto& method = cf.methods[methodIndex];
        if (utf8Text(cf, method.nameCp) != name || utf8Text(cf, method.descCp) != descriptor)
            continue;
        auto info = makeMethodInfo(*held, method, methodIndex, lvtJarFallback);
        if (info.is_err()) return ca::core::Err(std::move(info).unwrap_err());
        MethodBytecode result;
        result.method = std::move(info).unwrap();
        if (support::ensureCode(cf, method) != nullptr) {
            auto promoted = jbc::ir::promoteMethod(*held, methodIndex);
            if (promoted.is_err())
                return ca::core::Err(error(WorkspaceError::DataLoss, "get method bytecode",
                                           "method body cannot be promoted"));
            auto raw = promoted.unwrap()->dom.exportRaw();
            if (raw.is_err())
                return ca::core::Err(error(WorkspaceError::DataLoss, "get method bytecode",
                                           "method body cannot be exported"));
            const auto insns = std::move(raw).unwrap();
            ca::i32    index = 0;
            for (const auto& insn : insns) {
                InstructionInfo item;
                item.index  = index++;
                item.opcode = static_cast<ca::i32>(insn.op);
                const char* mnemonic = jbc::core::mnemonicOf(insn.op);
                item.mnemonic = ca::str::Utf8String::from_cstr(mnemonic != nullptr ? mnemonic : "?");
                item.nodeType = utf8(instructionNodeType(insn.op));
                result.instructions.push_back(std::move(item));
            }
        }
        return ca::core::Ok(std::move(result));
    }
    return ca::core::Err(error(WorkspaceError::NotFound, "get method bytecode",
                               owner + "." + name + descriptor));
}

jasm::Result<jasm::JvmJasmDisassembly> getJasm(model::ResourceContainer& container,
                                               ca::str::Utf8StringRef   uriRef)
{
    const std::string uri = uriRef.to_std_string();
    jasm::ResourceJasmService service(container);
    return service.disassemble(uri);
}

jasm::Result<jasm::JvmJasmMethodDisassembly> getMethodJasm(const model::ResourceContainer& container,
                                                           ca::str::Utf8StringRef ownerRef,
                                                           ca::str::Utf8StringRef nameRef,
                                                           ca::str::Utf8StringRef descriptorRef)
{
    const std::string owner = ownerRef.to_std_string();
    auto              uri   = resolveClassUri(container, owner);
    if (uri.is_err()) {
        return jasmFailure<jasm::JvmJasmMethodDisassembly>(std::move(uri).unwrap_err().message);
    }
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        container.getResource(uri.unwrap()));
    if (resource == nullptr)
        return jasmFailure<jasm::JvmJasmMethodDisassembly>("Class resource not found: " + owner);
    return jasm::JvmJasmService{}.disassembleMethod(resource->get(), nameRef, descriptorRef);
}

bool lvtJarFallbackOf(const model::ResourceContainer& container)
{
    mjbase::analysis::LvtShapeStats stats;
    // 全容器 class 扫描 LVT 行集（与 ReferenceScan::build 同纪律：净化预检
    // 不过的损坏 class 跳过、会话打不开跳过）。这里只需逐名形态判定，行集
    // 不落任何存储。
    container.visitResources([&](std::string_view, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        if (!spi::sanitizeClassFile(classResource->get()).empty()) return;
        auto session = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(classResource->get()));
        if (session.is_err()) return;
        auto                  held = std::move(session).unwrap();
        jbc::core::ClassFile& cf   = held->backing();
        for (auto& method : cf.methods) {
            jbc::core::CodeAttr* code = support::ensureCode(cf, method);
            if (code == nullptr) continue;
            std::vector<LocalVariableInfo> locals;
            support::collectLocalVariables(cf, *code, locals);
            for (const auto& local : locals) {
                ++stats.total;
                if (!mjbase::analysis::lvtNameTrustworthy(local.name.ref())) ++stats.invalid;
            }
        }
    });
    return mjbase::analysis::lvtJarFallback(stats);
}

WorkspaceResult<std::shared_ptr<jbc::ir::ClassSession>> sessionOf(const std::vector<ca::u8>& bytes,
                                                                  const std::string& operation)
{
    auto parsed = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (parsed.is_err())
        return ca::core::Err(
            error(WorkspaceError::DataLoss, operation, "class bytes cannot be parsed"));
    return ca::core::Ok(std::move(parsed).unwrap());
}

FieldInfo makeFieldInfo(jbc::core::ClassFile& cf, jbc::core::MemberInfo& field)
{
    FieldInfo output;
    output.name             = utf8(utf8Text(cf, field.nameCp));
    output.descriptor       = utf8(utf8Text(cf, field.descCp));
    output.signature        = utf8(support::signatureText(cf, field.attrs));
    output.access           = static_cast<ca::i32>(field.access);
    output.constantValue    = support::fieldConstantValue(cf, field);
    support::collectAnnotations(cf, field.attrs, output.annotations);
    return output;
}

std::string instructionNodeType(jbc::core::Op op)
{
    const char* mnemonic = jbc::core::mnemonicOf(op);
    const std::string name = mnemonic != nullptr ? mnemonic : "";

    const auto isAnyOf = [&name](std::initializer_list<std::string_view> candidates) {
        return std::any_of(candidates.begin(), candidates.end(), [&](std::string_view item) {
            return name == item;
        });
    };
    if (name == "invokedynamic") return "invokedynamic";
    if (name.rfind("invoke", 0) == 0) return "invoke";
    if (isAnyOf({"getfield", "getstatic", "putfield", "putstatic"})) return "field";
    if (isAnyOf({"ldc", "ldc_w", "ldc2_w"})) return "ldc";
    if (isAnyOf({"new", "anewarray", "checkcast", "instanceof"})) return "cp_ref";
    if (name == "multianewarray") return "multianewarray";
    if (name == "iinc") return "iinc";
    //数组元素加载/存储（两字符类型前缀 iaload/aastore 等）：先于局部变量
    //load/store 判定——裸名 aload/astore 属数组族；带槽位的 aload_N/astore_N
    //全名不命中数组表，剥后缀后走局部变量分支。
    static constexpr std::array<std::string_view, 16> kArrayOps = {
        "iaload", "laload", "faload", "daload", "aaload", "baload", "caload", "saload",
        "iastore", "lastore", "fastore", "dastore", "aastore", "bastore", "castore",
        "sastore"};
    for (const auto& arrayOp : kArrayOps) {
        if (name == arrayOp) return "array";
    }
    //局部变量加载/存储族：类型前缀（i/l/f/d/a）+ load/store；带槽位的短格式
    //（aload_0 等）先剥掉数字后缀再匹配。
    auto baseName = name;
    const auto underscore = name.rfind('_');
    if (underscore != std::string::npos &&
        std::all_of(name.begin() + static_cast<std::ptrdiff_t>(underscore) + 1, name.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; }))
        baseName = name.substr(0, underscore);
    if (baseName.size() == 5 && baseName.compare(1, 4, "load") == 0 &&
        std::string("ilfda").find(baseName[0]) != std::string::npos)
        return "var";
    if (baseName.size() == 6 && baseName.compare(1, 5, "store") == 0 &&
        std::string("ilfda").find(baseName[0]) != std::string::npos)
        return "var";
    if (name == "ret") return "var";
    if (name.rfind("iconst", 0) == 0 || name.rfind("lconst", 0) == 0 ||
        name.rfind("fconst", 0) == 0 || name.rfind("dconst", 0) == 0 ||
        isAnyOf({"aconst_null", "bipush", "sipush"}))
        return "const";
    switch (jbc::core::shapeOf(op)) {
    case jbc::core::Shape::Branch2:
    case jbc::core::Shape::Branch4: return "jump";
    case jbc::core::Shape::TableSwitch: return "tableswitch";
    case jbc::core::Shape::LookupSwitch: return "lookupswitch";
    case jbc::core::Shape::None: return "insn";
    case jbc::core::Shape::Imm1:
    case jbc::core::Shape::Imm2:
    case jbc::core::Shape::Atype1: return "int";
    case jbc::core::Shape::Local1: return "var";
    case jbc::core::Shape::Iinc1: return "iinc";
    default: break;
    }
    return "insn";
}

std::string fingerprintBytes(const std::vector<ca::u8>& bytes)
{
    constexpr ca::u64 kOffsetBasis = 14695981039346656037ULL;
    ca::u64           state        = kOffsetBasis;
    hashFingerprintLength(state, bytes.size());
    hashFingerprintBytes(state, bytes.data(), bytes.size());
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << state;
    return output.str();
}

WorkspaceResult<std::string> fingerprintContainer(const model::ResourceContainer& container)
{
    constexpr ca::u64        kOffsetBasis = 14695981039346656037ULL;
    ca::u64                  state        = kOffsetBasis;
    std::vector<std::string> uris;
    container.visitResources(
        [&](std::string_view uri, const model::IResource&) { uris.push_back(std::string(uri)); });
    std::sort(uris.begin(), uris.end());
    for (const auto& uri : uris) {
        const model::IResource* resource = container.getResource(uri);
        if (resource == nullptr) continue;
        auto bytes = bytesOf(*resource);
        if (bytes.is_err()) return ca::core::Err(std::move(bytes).unwrap_err());
        const auto& content = bytes.unwrap();
        hashFingerprintLength(state, uri.size());
        hashFingerprintBytes(state, reinterpret_cast<const ca::u8*>(uri.data()), uri.size());
        hashFingerprintLength(state, content.size());
        hashFingerprintBytes(state, content.data(), content.size());
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << state;
    return ca::core::Ok(output.str());
}

}   // namespace mjt::workspace
