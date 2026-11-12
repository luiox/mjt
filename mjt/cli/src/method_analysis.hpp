/// @file        method_analysis.hpp
/// @brief       cfg / dataflow 两命令共享的方法分析底座：.jar / .class 输入 → jbc ClassSession → 目标方法选择 → promote。
///
/// 输入双形态：.jar 走 workspace（classInfo 内名 / URI 双解析 + readClassBytes），其余一律按 .class 字节直读；两条路都经 makeSessionFromBytes 解析。方法 spec = "name" 或 "name:descriptor"，同名多命中报 ambiguous；未指定 spec 取第一个带 Code 属性的方法（无则退第一个方法，空方法体由消费方按自身契约处置）。
///
/// 寿命：Target::code 是 CodeUnitRef 保活句柄（内部持 ClassSession shared_ptr），CP 语义视图与符号池在 Target 存活期内一直可解析。
#pragma once

#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "libca/str/utf8_string.hpp"

#include "jbc/core/error.hpp"
#include "jbc/ir/error.hpp"
#include "jbc/ir/units.hpp"

#include "shared/file_io.hpp"
#include "shared/i18n_errors.hpp"
#include "workspace/analysis_ops.hpp"
#include "workspace/workspace.hpp"
#include "workspace/workspace_types.hpp"

namespace mjt::cli::method_analysis {

/// @brief 加载失败：usage = true 走退出码 1（用法：选择器无命中 / 缺 -c / 方法无方法体），false 走退出码 2（运行：jar 打开 / 字节读取 / class 解析失败）。message 已按 i18n 渲染。
struct LoadFailure
{
    bool        usage = false;
    std::string message;
};

/// @brief 加载完成的目标方法（方法体可空：abstract / native 无 Code 属性）。
struct Target
{
    jbc::ir::CodeUnitRef code;   ///< 已升格方法体；无方法体时空句柄
    std::string className;
    std::string methodName;
    std::string descriptor;
    bool    instance = false;    ///< 非 ACC_STATIC
    bool    hasBody  = false;    ///< 带 Code 属性
    ca::u32 access   = 0;        ///< 方法 access 旗标（JVMS 位掩码）
};

/// Utf8String/UTF-8 std::string → 借用视图（disasm / hexdump 命令同此构造）。
inline ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// 后缀大小写不敏感（.JAR 同样走 jar 路）。
inline bool endsWithNoCase(const std::string& value, const char* suffix)
{
    const std::size_t n = std::strlen(suffix);
    if (value.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(value[value.size() - n + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i])))
            return false;
    }
    return true;
}

/// LoadFailure 便捷构造（i18n 渲染；@p errPrefix 已含 ".err."）。
inline LoadFailure loadFailure(bool usage, const std::string& key,
                               std::initializer_list<std::string_view> args = {})
{
    LoadFailure failure;
    failure.usage   = usage;
    failure.message = shared::trfStr(key.c_str(), args);
    return failure;
}

/// .class 直读时的类名推断：取最后一段路径，剥 .class 后缀。
inline std::string inferredClassName(const std::string& inputPath)
{
    const auto slash = inputPath.find_last_of("/\\");
    const auto fname = slash == std::string::npos ? inputPath : inputPath.substr(slash + 1);
    if (fname.size() >= 6 && fname.compare(fname.size() - 6, 6, ".class") == 0)
        return fname.substr(0, fname.size() - 6);
    return fname;
}

/// 从类字节构建会话（统一解析入口）；失败按 invalid_class_file 返回真实原因（read 段 / session 段的错误码原文，不跨族有损映射）。
inline ca::Result<std::shared_ptr<jbc::ir::ClassSession>, LoadFailure> sessionFromBytes(
    const std::vector<ca::u8>& bytes, const std::string& inputPath,
    const std::string& errPrefix)
{
    const auto opened = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView(bytes.data(), bytes.size()));
    if (opened.is_err()) {
        const auto  error  = std::move(opened).unwrap_err();
        const char* detail = error.stage == jbc::ir::SessionError::Stage::Read
                                 ? jbc::core::to_string(error.read)
                                 : jbc::ir::to_string(error.session);
        return ca::Err(loadFailure(false, errPrefix + "invalid_class_file",
                                   {inputPath, detail}));
    }
    return ca::Ok(std::move(opened).unwrap());
}

/// 方法 spec 解析："name" 或 "name:descriptor"（首个冒号即分界，方法描述符本身不含冒号）。
inline std::pair<std::string_view, std::string_view> splitMethodSpec(const std::string& spec)
{
    const auto colon = spec.find(':');
    if (colon == std::string::npos) return {spec, {}};
    return {std::string_view(spec).substr(0, colon), std::string_view(spec).substr(colon + 1)};
}

/// 目标方法选择：spec 空取第一个带 Code 的方法（无则退第一个方法）；spec 命中多个（desc 缺省按名匹配）报 ambiguous。返回方法逻辑下标，@p noBody 为 true 表示选中的方法没有 Code 属性。
/// @param errPrefix i18n 键前缀（含 ".err."，如 "mjt.cmd.cfg.err."）。
inline ca::Result<ca::usize, LoadFailure> selectMethod(const jbc::ir::ClassSession& session,
                                                       const std::string& methodSpec,
                                                       const std::string& className,
                                                       const std::string& errPrefix,
                                                       bool& noBody)
{
    const jbc::ir::ClassUnit& unit    = session.unit();
    const auto&               methods = unit.methods;
    const auto&               symbols = unit.symbols();

    if (methodSpec.empty()) {
        for (ca::usize i = 0; i < methods.size(); ++i) {
            if (jbc::ir::hasCode(session, i)) return ca::Ok(i);
        }
        noBody = !methods.empty();
        return ca::Ok(ca::usize{0});
    }

    const auto [name, desc] = splitMethodSpec(methodSpec);
    ca::usize found         = jbc::ir::kNoMethod;
    for (ca::usize i = 0; i < methods.size(); ++i) {
        if (symbols.str(methods[i].name) != name) continue;
        if (!desc.empty() && symbols.str(methods[i].desc) != desc) continue;
        if (found != jbc::ir::kNoMethod)
            return ca::Err(loadFailure(true, errPrefix + "ambiguous_method", {methodSpec}));
        found = i;
    }
    if (found == jbc::ir::kNoMethod)
        return ca::Err(loadFailure(true, errPrefix + "method_not_found",
                                   {methodSpec, className}));
    noBody = !jbc::ir::hasCode(session, found);
    return ca::Ok(found);
}

/// 统一入口：输入（.jar 经 workspace / 其余按 .class 直读）→ ClassSession → 目标方法选择 → promote。@p keyPrefix 是 "mjt.cmd.cfg" / "mjt.cmd.dataflow" 族共享错误键的命令前缀。
/// @param requireBody true（dataflow）：无方法体按 no_code 用法错误拒绝（退出码 1）；
///                    false（cfg）：照常交付 Target.hasBody = false。
inline ca::Result<Target, LoadFailure> load(const std::string& inputPath,
                                            const std::string& className,
                                            const std::string& methodSpec,
                                            const char*        keyPrefix,
                                            bool               requireBody)
{
    const std::string errPrefix = std::string(keyPrefix) + ".err.";
    std::vector<ca::u8> bytes;

    if (endsWithNoCase(inputPath, ".jar")) {
        if (className.empty())
            return ca::Err(loadFailure(true, errPrefix + "class_required_for_jar"));
        workspace::WorkspaceBase ws;
        auto opened = ws.open(refOf(inputPath));
        if (opened.is_err()) {
            workspace::WorkspaceErrorInfo info = std::move(opened).unwrap_err();
            return ca::Err(loadFailure(false, errPrefix + "open_failed", {info.message}));
        }
        // 类选择（与 disasm 命令一致）：内名 / URI 双解析；NotFound → 用法 1，其余（条目损坏 / 解压失败）→ 运行 2。
        auto selected = workspace::classInfo(*ws.workingContainer(), refOf(className),
                                             ws.lvtJarFallbackFlag());
        if (selected.is_err()) {
            workspace::WorkspaceErrorInfo info = std::move(selected).unwrap_err();
            const bool missing = info.code == workspace::WorkspaceError::NotFound;
            return ca::Err(loadFailure(missing, errPrefix + "class_not_readable",
                                       {className, info.message}));
        }
        auto read = workspace::readClassBytes(*ws.workingContainer(),
                                              std::move(selected).unwrap().resourceUri.ref());
        if (read.is_err()) {
            workspace::WorkspaceErrorInfo info = std::move(read).unwrap_err();
            return ca::Err(loadFailure(false, errPrefix + "class_not_readable",
                                       {className, info.message}));
        }
        bytes = std::move(read).unwrap().bytes;
    }
    else {
        // .class（或任意文件名输入）直读字节：不存在的路径在 main 的用法校验已拦；此处覆盖读失败 / 空文件（权限 / 竞态删除）。
        bytes = mjt::readWholeFile(inputPath);
        if (bytes.empty())
            return ca::Err(loadFailure(false, errPrefix + "read_failed", {inputPath}));
    }

    auto session = sessionFromBytes(bytes, inputPath, errPrefix);
    if (session.is_err()) return ca::Err(std::move(session).unwrap_err());
    auto held = std::move(session).unwrap();

    const std::string displayName = className.empty() ? inferredClassName(inputPath) : className;
    if (held->unit().methods.empty())
        return ca::Err(loadFailure(false, errPrefix + "no_methods", {displayName}));

    bool noBody = false;
    auto index  = selectMethod(*held, methodSpec, className, errPrefix, noBody);
    if (index.is_err()) return ca::Err(std::move(index).unwrap_err());
    const ca::usize methodIdx = index.unwrap();

    const jbc::ir::ClassUnit&  method = held->unit();
    const jbc::ir::MethodUnit& entry  = method.methods[methodIdx];
    const auto&                symbols = method.symbols();

    Target target;
    target.className  = displayName;
    target.methodName = std::string(symbols.str(entry.name));
    target.descriptor = std::string(symbols.str(entry.desc));
    target.instance   = (entry.access & 0x0008) == 0;   // ACC_STATIC
    target.access     = entry.access;
    target.hasBody    = !noBody;

    if (noBody) {
        if (requireBody) {
            return ca::Err(loadFailure(true, errPrefix + "no_code",
                                       {methodSpec.empty() ? target.methodName : methodSpec,
                                        target.className}));
        }
        return ca::Ok(std::move(target));   // cfg：空方法体走空图输出
    }

    auto promoted = jbc::ir::promoteMethod(*held, methodIdx);
    if (promoted.is_err()) {
        const auto error = std::move(promoted).unwrap_err();
        return ca::Err(loadFailure(false, errPrefix + "invalid_class_file",
                                   {inputPath, jbc::ir::to_string(error)}));
    }
    target.code = std::move(promoted).unwrap();
    return ca::Ok(std::move(target));
}

}   // namespace mjt::cli::method_analysis
