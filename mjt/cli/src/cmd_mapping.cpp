/// @file        cmd_mapping.cpp
/// @brief       `mjt mapping` 命令核心：mapping 格式互转（mjbase 统一中间
///              表示）+ 可选的 mapping 应用重映射 jar（shared 层 remapJar）。
///
/// 格式名解析：注册表标准名（SRG / Tiny-V2 / Proguard / Tiny-V1 / Simple /
/// Enigma / R8 / YGuard / Allatori / Shadow-Alias / Jadx (Legacy)）按大小写
/// 不敏感唯一匹配；互转语义 = parse(from) → IntermediateMapping →
/// format(to)，能力面与格式宽容语义全部在 mjbase mapping 模块。
#include "commands.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/mapping/mapping_format_registry.hpp"
#include "shared/file_io.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/jar_remapper.hpp"

namespace mjt::cli {
namespace {

/// mjbase mapping 解析/生成错误 → 短文本（MappingError 三个值，无 to_string）。
const char* mappingErrorText(mjbase::mapping::MappingError error)
{
    switch (error) {
    case mjbase::mapping::MappingError::InvalidText: return "invalid mapping text";
    case mjbase::mapping::MappingError::UnknownFormat: return "unknown mapping format";
    case mjbase::mapping::MappingError::Io: return "mapping io failure";
    }
    return "mapping failure";
}

/// 比较名归一：小写化（ASCII；格式名全是 ASCII）。
std::string folded(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

}   // namespace

ca::Result<std::string, MappingError> resolveMappingFormatName(const std::string& name)
{
    const auto registry = mjbase::mapping::MappingFormatRegistry::withStandardFormats();
    const auto foldedName = folded(name);

    // 大小写不敏感匹配注册名；注册表对同一实现保留了多别名（如 "Tiny-V2"/
    // "Tiny-v2"、"Proguard"/"ProGuard"），折叠后同名命中取 formatNames() 确定
    // 序第一个（canonical 名），未注册名 fail-loud。
    std::string canonical;
    for (const auto registered : registry.formatNames()) {
        if (folded(registered.to_std_string()) == foldedName) {
            canonical = registered.to_std_string();
            break;
        }
    }
    if (canonical.empty()) {
        std::string known;
        for (const auto registered : registry.formatNames()) {
            if (!known.empty()) known += ", ";
            known += registered.to_std_string();
        }
        MappingError error;
        error.kind    = MappingError::Kind::UnknownFormat;
        error.message = shared::trfStr("mjt.cli.err.mapping_unknown_format", {name, known});
        return ca::Err(std::move(error));
    }
    return ca::Ok(std::move(canonical));
}

ca::Result<std::string, MappingError> convertMappingText(const std::string& mappingText,
                                                         const std::string& fromName,
                                                         const std::string& toName)
{
    auto from = resolveMappingFormatName(fromName);
    if (from.is_err()) return ca::Err(std::move(from).unwrap_err());
    auto to = resolveMappingFormatName(toName);
    if (to.is_err()) return ca::Err(std::move(to).unwrap_err());

    auto registry = mjbase::mapping::MappingFormatRegistry::withStandardFormats();
    auto parsed = registry.parse(
        ca::str::Utf8StringRef::from_cstr(from.unwrap().c_str()),
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(mappingText.data()),
                                          mappingText.size()));
    if (parsed.is_err()) {
        MappingError error;
        error.kind    = MappingError::Kind::ParseFailed;
        error.message = shared::trfStr("mjt.cli.err.mapping_parse_failed",
                                       {mappingErrorText(std::move(parsed).unwrap_err())});
        return ca::Err(std::move(error));
    }
    auto mappings = std::move(parsed).unwrap();
    auto formatted = registry.format(ca::str::Utf8StringRef::from_cstr(to.unwrap().c_str()),
                                     mappings);
    if (formatted.is_err()) {
        MappingError error;
        error.kind    = MappingError::Kind::FormatFailed;
        error.message = shared::trfStr("mjt.cli.err.mapping_format_failed",
                                       {mappingErrorText(std::move(formatted).unwrap_err())});
        return ca::Err(std::move(error));
    }
    return ca::Ok(std::move(formatted).unwrap().to_std_string());
}

ca::Result<bool, MappingError> remapJarWithMapping(const RemapJarRequest& request)
{
    mjt::JarRemapRequest remap;
    remap.inputPath  = request.jarPath;
    remap.outputPath = request.outputPath;
    remap.mappingPath = request.mappingPath;
    remap.format     = request.format;
    remap.policy     = request.policy.empty() ? "bytecode-only" : request.policy;

    auto remapped = mjt::remapJar(remap);
    if (remapped.is_err()) {
        MappingError error;
        error.kind    = MappingError::Kind::Io;
        error.message = shared::trfStr("mjt.cli.err.remap_failed",
                                       {std::move(remapped).unwrap_err().message()});
        return ca::Err(std::move(error));
    }
    return ca::Ok(std::move(remapped).unwrap());
}

}   // namespace mjt::cli
