#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/container_stack.hpp"
#include "mjbase/model/resource_container.hpp"

namespace mjt::workspace {

/// @file container_registry.hpp
/// @brief 会话级容器登记表：一个挂载容器一条记录，workspace 支撑面、
///        角色化容器栈与工程登记共用同一事实源。
///
/// id 是 containerStack 的挂载键（规范化绝对路径），supportId 是 workspace
/// 侧 detach 句柄（support-N）。容器对象为共享所有权：MCP 挂载路径只导入
/// 一次，workspace 的分析扫描面直接复用同一字节视图，不再各持一份容器。
struct ContainerRecord final
{
    std::string                                         id;           ///< 容器 id / 栈挂载键
    std::string                                         supportId;    ///< workspace detach 句柄
    std::string                                         path;         ///< 规范化绝对路径
    std::string                                         fingerprint;  ///< 内容指纹
    mjbase::model::ContainerRole                        role =
        mjbase::model::ContainerRole::Library;                        ///< library/runtime 角色
    std::string                                         roleLabel = "support";  ///< support/library/runtime 标签
    std::shared_ptr<mjbase::model::ResourceContainer>   container;  ///< 共享容器字节视图
};

/// @brief 会话内唯一的容器登记面：登记、查重、摘除与句柄分配。
class ContainerRegistry final
{
public:
    std::vector<ContainerRecord>&       records() noexcept { return records_; }
    const std::vector<ContainerRecord>& records() const noexcept { return records_; }

    /// @brief 分配一个会话内单调的 support 句柄。
    std::string nextSupportId() { return "support-" + std::to_string(nextId_++); }

    /// @brief 打开新主资源时清空全部挂载并复位句柄计数。
    void clear()
    {
        records_.clear();
        nextId_ = 1;
    }

    const ContainerRecord* findBySupportId(std::string_view id) const
    {
        return findIf([&](const ContainerRecord& record) { return record.supportId == id; });
    }

    const ContainerRecord* findByContainerId(std::string_view id) const
    {
        return findIf([&](const ContainerRecord& record) { return record.id == id; });
    }

    const ContainerRecord* findByPath(std::string_view path) const
    {
        return findIf([&](const ContainerRecord& record) { return record.path == path; });
    }

    const ContainerRecord* findByFingerprint(std::string_view fingerprint) const
    {
        return findIf(
            [&](const ContainerRecord& record) { return record.fingerprint == fingerprint; });
    }

    /// @brief 登记一条记录；路径或内容重复返回 false，登记表不变。
    bool attach(ContainerRecord record)
    {
        if (findByPath(record.path) != nullptr || findByFingerprint(record.fingerprint) != nullptr)
            return false;
        records_.push_back(std::move(record));
        return true;
    }

    /// @brief 按 support 句柄摘除；未命中返回 nullopt。
    std::optional<ContainerRecord> detachBySupportId(std::string_view id)
    {
        const auto found = std::find_if(
            records_.begin(), records_.end(),
            [&](const ContainerRecord& record) { return record.supportId == id; });
        if (found == records_.end()) return std::nullopt;
        ContainerRecord removed = std::move(*found);
        records_.erase(found);
        return removed;
    }

private:
    template <typename Pred>
    const ContainerRecord* findIf(Pred pred) const
    {
        for (const auto& record : records_) {
            if (pred(record)) return &record;
        }
        return nullptr;
    }

    std::vector<ContainerRecord> records_;
    ca::u64                      nextId_ = 1;
};

}   // namespace mjt::workspace
