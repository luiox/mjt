#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "service.hpp"

namespace mjt::mcp {

///@brief libjbc 保真往返校验工具（libjbc 包消费试点）。
///
///`class_fidelity_check`：读工作副本 class 字节 → libjbc 解析为 DOM →
///全程不触碰（不解码任何属性、不 mutate）→ 保真写回 → 字节恒等断言 +
///结构摘要（类名/成员计数/属性清单/解析状态）。libjbc 的核心保真契约
///（未触碰属性 copy 源 span，触碰才重编码）在此直接可见，作为后续替换
///libjbytecode（legacy 冻结包）解析路径的立足点。
std::optional<Json> callFidelityTool(const std::string&                name,
                                     const Json&                      arguments,
                                     mjbase::workspace::IWorkspace* current);

}   //namespace mjt::mcp
