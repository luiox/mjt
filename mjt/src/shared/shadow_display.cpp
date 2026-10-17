//shadow_display：shadow name 显示层命令侧适配（luiox/）。
//
//路径双轨解析 + sidecar 载入 + 渲染便利封装；表本体与生成/清理在
//mbase/renamer（cmd 与 mcp 共用），本文件只做 mjt 命令层的 std::string 桥接。
#include "shared/shadow_display.hpp"

#include <cstring>
#include <filesystem>

#include "libca/i18n/i18n.hpp"
#include "mbase/cli/output_sink.hpp"
#include "mjbase/mapping/identifier_util.hpp"
#include "mjbase/mapping/shadow_alias_format.hpp"

using mjbase::mapping::IdentifierUtil;
using ca::str::Utf8StringRef;

namespace {

///@brief std::string → Utf8StringRef 临时视图：mjbase 的文件路径面仍是
///Utf8StringRef（路径可能含非 UTF-8 字节，原始字节直通不经 ACP）。
Utf8StringRef refOf(const std::string& value)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

}   //namespace

namespace mjt {

std::string resolveShadowPath(const std::string& jarPath, const std::string& explicitPath)
{
    if (!explicitPath.empty()) return explicitPath;
    return jarPath + ".mjt-shadow";
}

std::string internalNameOfUri(const std::string& uri)
{
    const char*     kSuffix   = ".class";
    const size_t    suffixLen = std::strlen(kSuffix);
    if (uri.size() > suffixLen && uri.compare(uri.size() - suffixLen, suffixLen, kSuffix) == 0) {
        return uri.substr(0, uri.size() - suffixLen);
    }
    return uri;
}

std::string ShadowDisplay::className(const std::string& internalName) const
{
    if (!active()) return IdentifierUtil::printReadable(internalName);
    //mjbase 渲染产物按值返回 std::string（旧栈为 Utf8String，需 to_std_string）。
    return table.renderClass(internalName);
}

std::string ShadowDisplay::memberName(const std::string& owner, const std::string& name,
                                      const std::string& desc) const
{
    if (!active()) return IdentifierUtil::printReadable(name);
    return table.renderMemberName(owner, name, desc);
}

std::string ShadowDisplay::fieldLine(const std::string& owner, const std::string& name,
                                     const std::string& desc) const
{
    if (!active()) {
        //既有口径：name + " " + desc 拼接后整体过 printReadable（空格转 \u0020）。
        std::string composed = name;
        composed += " ";
        composed += desc;
        return IdentifierUtil::printReadable(composed);
    }
    return memberName(owner, name, desc) + " " + IdentifierUtil::printReadable(desc);
}

std::string ShadowDisplay::member(const std::string& owner, const std::string& name,
                                  const std::string& desc) const
{
    if (!active()) {
        return IdentifierUtil::printReadable(owner) + "." + IdentifierUtil::printReadable(name) +
               ":" + IdentifierUtil::printReadable(desc);
    }
    return table.renderMember(owner, name, desc);
}

std::string ShadowDisplay::memberText(const std::string& composed) const
{
    if (!active()) return composed;
    //各段已 printReadable：先反转义回原始字节，再走结构化渲染。
    const std::string raw    = IdentifierUtil::unescapeUnicode(composed);
    const auto        dotPos = raw.find('.');
    const auto        colonPos = raw.rfind(':');
    if (dotPos == std::string::npos || colonPos == std::string::npos || colonPos < dotPos) {
        return composed;
    }
    const std::string owner = raw.substr(0, dotPos);
    const std::string name  = raw.substr(dotPos + 1, colonPos - dotPos - 1);
    const std::string desc  = raw.substr(colonPos + 1);
    if (owner.empty() || name.empty()) return composed;
    return member(owner, name, desc);
}

ShadowDisplay loadShadowDisplay(const std::string& jarPath, const std::string& explicitPath,
                                bool noShadow)
{
    ShadowDisplay display;
    display.enabled = !noShadow;

    const auto path     = resolveShadowPath(jarPath, explicitPath);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return display;

    auto mappings = mjbase::mapping::readShadowAliasFile(refOf(path));
    if (mappings.is_err()) {
        auto       error = std::move(mappings).unwrap_err();
        mjt::base::cli::err().printf(
            "%s\n",
            ca::i18n::trf("mjt.cmd.shadow.err.sidecar_invalid",
                          {path, error.message.to_std_string()})
                .c_str());
        return display;
    }
    display.table = mjbase::renamer::ShadowAliasTable::fromMapping(std::move(mappings).unwrap());
    return display;
}

std::string rewriteJasmForDisplay(const ShadowDisplay& display, const std::string& jasmText,
                                  const std::string& ownerClass)
{
    //mjbase 侧入参/产物都是 std::string_view/std::string，不再需要
    //Utf8String 中转与 .ref().to_std_string() 回传。
    return mjbase::renamer::rewriteJasmForDisplay(jasmText, display.table, ownerClass);
}

}   //namespace mjt
