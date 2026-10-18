#include "fidelity.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <jbc/jbc.hpp>

#include "json.hpp"
#include "shared/status_convert.hpp"
#include "tool/shadow_slot.hpp"

namespace mjt::mcp {

using namespace detail;
using mjbase::workspace::IWorkspace;

std::optional<Json> callFidelityTool(
    const std::string& name, const Json& arguments, IWorkspace* current)
{
    if (name != "class_fidelity_check") return std::nullopt;

    //T2 name 槽：与 class_get/class_bytes 同一 class 选择器语义（Ref/raw 二选一，
    //sidecar 活跃时 raw 核表）。
    std::string className;
    Json        slotError;
    if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError))
        return std::move(slotError);

    auto bytes = current->readClassBytes(className);
    if (bytes.is_err()) {
        const auto err = shared::toCaStatus(bytes.unwrap_err());
        return toolError("read class bytes failed", &err);
    }
    auto value = std::move(bytes).unwrap();

    //libjbc 会话解析：ReadOptions 默认保留源字节（copy-through 保真前提）。
    auto read = jbc::core::readClass(jbc::core::ByteView{value.bytes});
    if (read.is_err()) {
        return toolError(std::string("libjbc parse failed: ") +
                         jbc::core::to_string(read.unwrap_err()));
    }
    auto        parsed = std::move(read).unwrap();
    const auto& cf     = *parsed.file;

    //不触碰写回：不 decodeAttr、不 mutate，全部属性保持 Untouched，写回走
    //copy 路径——恒等断言检验的正是 libjbc 的 copy-through 保真契约。
    auto written = jbc::core::writeClass(cf);
    if (written.is_err()) {
        return toolError(std::string("libjbc write failed: ") +
                         jbc::core::to_string(written.unwrap_err()));
    }
    auto rewritten = std::move(written).unwrap();

    const bool identical = rewritten.size() == value.bytes.size() &&
                           std::equal(rewritten.begin(), rewritten.end(), value.bytes.begin());
    Json result = {{"resource_uri", value.resourceUri},
                   {"internal_name", std::string(cf.thisName())},
                   {"fingerprint", value.fingerprint},
                   {"libjbc_version", jbc::get_lib_version()},
                   {"parse_state", jbc::core::to_string(cf.state())},
                   {"diagnostic_count", cf.diagnostics().count()},
                   {"identical", identical},
                   {"size_in", value.bytes.size()},
                   {"size_out", rewritten.size()},
                   {"major", cf.major},
                   {"minor", cf.minor},
                   {"super_class", std::string(cf.superName())},
                   {"interface_count", cf.interfaces.size()},
                   {"field_count", cf.fields.size()},
                   {"method_count", cf.methods.size()}};
    if (!identical) {
        //字节恒等破坏时给出首个分歧偏移，帮助定位重编码差异。
        const auto common = std::min(rewritten.size(), value.bytes.size());
        ca::usize  offset = 0;
        while (offset < common && rewritten[offset] == value.bytes[offset]) ++offset;
        result["first_mismatch_offset"] = offset;
    }
    Json attributes = Json::array();
    for (const auto& attr : cf.attrs) attributes.push_back(std::string(cf.utf8(attr.nameCp)));
    result["class_attributes"] = std::move(attributes);
    ca::usize fieldAttributeCount  = 0;
    for (const auto& field : cf.fields) fieldAttributeCount += field.attrs.size();
    ca::usize methodAttributeCount = 0;
    for (const auto& method : cf.methods) methodAttributeCount += method.attrs.size();
    result["field_attribute_count"]  = fieldAttributeCount;
    result["method_attribute_count"] = methodAttributeCount;
    return toolResult(std::move(result));
}

}   //namespace mjt::mcp
