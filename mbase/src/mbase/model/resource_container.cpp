#include "resource_container.hpp"
#include "resource_io.hpp"

#include <cstdio>
#include "libca/log/log_macros.hpp"

namespace mjt::base {

//委托给底层 unordered_map 的常见操作
void ResourceContainer::addResource(const std::string& uri, std::unique_ptr<IResource> resource)
{
    //重复条目：按 JVM 类加载语义 first-wins 保留首条目（issue ）。含重复
    //条目的 jar 常为恶意构造——藏在后条目里的实现会被 JVM 忽略，last-wins 导入
    //却让工具分析后条目，造成「工具分析的对象」与「实际加载的对象」错配。
    //记录 + stderr 告警，导入方可用 duplicateResourceUris 上报；
    //有意替换（候选重建、事务预览、undo/redo 回滚）请走 replaceResource。
    if (resources_.count(uri) != 0) {
        duplicateResourceUris_.push_back(uri);
        CA_LOG_WARN("[mbase] warning: duplicate jar entry '{}' (kept first occurrence, aligned with JVM first-wins class loading)",
                    uri.c_str());
        return;
    }
    resources_[uri] = std::move(resource);
}

bool ResourceContainer::hasResource(const std::string& uri) const
{
    return resources_.find(uri) != resources_.end();
}

IResource* ResourceContainer::getResource(const std::string& uri) const
{
    auto it = resources_.find(uri);
    return it != resources_.end() ? it->second.get() : nullptr;
}

void ResourceContainer::visitResources(
    const std::function<void(const std::string&, const IResource&)>& visitor) const
{
    for (const auto& [uri, res] : resources_) visitor(uri, *res);
}

//薄委托：导入/导出实际由 IResourceImporter / IResourceExporter 实现
void ResourceContainer::importResource(IResourceImporter* importer)
{
    beginImport();
    importer->importResource(*this);
}

void ResourceContainer::exportResource(IResourceExporter* exporter)
{
    exporter->exportResource(*this);
}

}   //namespace mjt::base
