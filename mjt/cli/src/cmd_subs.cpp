/// @file        cmd_subs.cpp
/// @brief       `mjt subs` 命令核心：单类成员清单（字段/方法：名、描述符、
///              access 旗标、native 标记）。
///
/// 地基：workspace classInfo 读面（单类直读，声明序保留）+ mjbase
/// classifyClassKind 类形态派生（classes / census 同源同果）。单类查询不走
/// censusMembers——那需要整 jar 分析库导入，单类成本不成比例。成员行 = 字段
/// 在前（censusMembers kind 编码序）、各自保持 classfile 声明序。纯文本面，
/// 不依赖反编译器。
#include "commands.hpp"

#include <string>
#include <utility>
#include <vector>

#include "mjbase/project/class_kind.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/json_util.hpp"
#include "shared/output_sink.hpp"
#include "workspace/workspace.hpp"
#include "workspace/workspace_types.hpp"

namespace mjt::cli {
namespace {

/// Utf8String/UTF-8 std::string → 借用视图（与 info / strings 同款构造）。
ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// SubsError 便捷构造（i18n 渲染）。
SubsError subsError(SubsError::Kind kind, const char* key,
                    std::initializer_list<std::string_view> args = {})
{
    SubsError error;
    error.kind    = kind;
    error.message = shared::trfStr(key, args);
    return error;
}

/// 方法行派生：ACC_NATIVE 位测试（kAccNative = 0x0100，JVMS 表 4.6）。
SubsMember methodRow(const workspace::MethodInfo& method)
{
    SubsMember row;
    row.kind       = "method";
    row.name       = method.name.to_std_string();
    row.descriptor = method.descriptor.to_std_string();
    row.access     = method.access;
    row.native     = (method.access & workspace::kAccNative) != 0;
    return row;
}

/// 字段行派生：native 恒 false（DbMemberCensusRow.native 字段口径同文）。
SubsMember fieldRow(const workspace::FieldInfo& field)
{
    SubsMember row;
    row.kind       = "field";
    row.name       = field.name.to_std_string();
    row.descriptor = field.descriptor.to_std_string();
    row.access     = field.access;
    row.native     = false;
    return row;
}

}   // namespace

ca::Result<SubsReport, SubsError> collectSubs(const std::string& jarPath,
                                              const std::string& internalName)
{
    workspace::WorkspaceBase ws;
    auto opened = ws.openJar(refOf(jarPath));
    if (opened.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(opened).unwrap_err();
        return ca::Err(subsError(SubsError::Kind::Runtime, "mjt.cli.subs.err.open_failed",
                                 {info.message}));
    }

    // 类选择器定位（URI / 内名双解析）：NotFound → 退出码 1，其余 → 退出码 2。
    auto selected = ws.classInfo(refOf(internalName));
    if (selected.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(selected).unwrap_err();
        return ca::Err(info.code == workspace::WorkspaceError::NotFound
                           ? subsError(SubsError::Kind::ClassNotFound,
                                       "mjt.cli.subs.err.class_not_found", {internalName})
                           : subsError(SubsError::Kind::Runtime, "mjt.cli.subs.err.open_failed",
                                       {info.message}));
    }
    // MethodInfo 携 move-only Utf8String：unwrap 走移动重载，一次取值。
    auto info = std::move(selected).unwrap();

    SubsReport report;
    report.jarPath      = jarPath;
    report.internalName = info.internalName.to_std_string();
    report.kind = std::string(mjbase::project::classKindName(
        mjbase::project::classifyClassKind(info.access, info.superName.ref())));
    report.members.reserve(info.fields.size() + info.methods.size());
    for (const auto& field : info.fields) report.members.push_back(fieldRow(field));
    for (const auto& method : info.methods) report.members.push_back(methodRow(method));
    report.fields  = static_cast<ca::i64>(info.fields.size());
    report.methods = static_cast<ca::i64>(info.methods.size());
    return ca::Ok(std::move(report));
}

void renderSubsText(const SubsReport& report, OutputSink& sink)
{
    sink.puts(shared::trfStr("mjt.cli.subs.title", {report.internalName}));
    sink.puts(shared::trfStr("mjt.cli.subs.class_kind", {report.kind}));
    sink.puts(shared::trfStr("mjt.cli.subs.counted",
                             {std::to_string(report.fields), std::to_string(report.methods)}));
    for (const auto& member : report.members) {
        std::string line = "  " + member.kind + " " + member.name + " " + member.descriptor +
                           " access=" + std::to_string(member.access);
        if (member.native) line += " native";
        sink.puts(line);
    }
}

void renderSubsJson(const SubsReport& report, OutputSink& sink)
{
    std::string out = "{\n";
    out += "  \"jar\": \"" + json::escape(report.jarPath) + "\",\n";
    out += "  \"class\": \"" + json::escape(report.internalName) + "\",\n";
    out += "  \"kind\": \"" + json::escape(report.kind) + "\",\n";
    out += "  \"fields\": " + std::to_string(report.fields) + ",\n";
    out += "  \"methods\": " + std::to_string(report.methods) + ",\n";
    out += "  \"members\": [";
    for (std::size_t index = 0; index < report.members.size(); ++index) {
        const auto& member = report.members[index];
        if (index != 0) out += ", ";
        out += "\n    {\"kind\": \"" + json::escape(member.kind) +
               "\", \"name\": \"" + json::escape(member.name) +
               "\", \"descriptor\": \"" + json::escape(member.descriptor) +
               "\", \"access\": " + std::to_string(member.access) +
               ", \"native\": " + (member.native ? "true" : "false") + "}";
    }
    if (!report.members.empty()) out += "\n  ";
    out += "]\n}";
    sink.puts(out);
}

}   // namespace mjt::cli
