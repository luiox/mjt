#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>

#include "resource_model.hpp"

namespace mjt::base {

class IResourceImporter;
class IResourceExporter;

///@brief 资源容器接口，统一管理多种类型的资源。
class IResourceContainer
{
public:
    virtual ~IResourceContainer() = default;
    ///@brief 添加资源，按 URI 索引。
    virtual void addResource(const std::string& uri, std::unique_ptr<IResource> resource) = 0;
    ///@brief 检查 URI 对应的资源是否存在。
    virtual bool hasResource(const std::string& uri) const = 0;
    ///@brief 获取 URI 对应的资源（原始指针，不转移所有权）。
    virtual IResource* getResource(const std::string& uri) const = 0;
    ///@brief 获取容器中资源总数。
    virtual std::size_t size() const = 0;
    ///@brief 清空所有资源。
    virtual void clear() = 0;
    ///@brief 遍历所有资源。
    virtual void visitResources(
        const std::function<void(const std::string&, const IResource&)>& visitor) const = 0;
    ///@brief 使用导入器向容器中导入资源。
    virtual void importResource(IResourceImporter* importer) = 0;
    ///@brief 使用导出器将容器中资源导出。
    virtual void exportResource(IResourceExporter* exporter) = 0;
};

///@brief 基于 unordered_map 的资源容器默认实现。
class ResourceContainer : public IResourceContainer
{
public:
    void        addResource(const std::string& uri, std::unique_ptr<IResource> resource) override;
    ///@brief 显式替换（或新增）资源，不触发重复导入告警。
    ///
    ///候选重建、事务预览、undo/redo 回滚等内部流程对已知 URI 的有意替换
    ///不属于"重复导入"语义，不应记入 duplicateResourceUris_/stderr 告警；
    ///外部导入路径（导入器、一次性导入）请继续使用 addResource。
    void replaceResource(const std::string& uri, std::unique_ptr<IResource> resource)
    {
        resources_[uri] = std::move(resource);
    }
    bool hasResource(const std::string& uri) const override;
    IResource*  getResource(const std::string& uri) const override;
    std::size_t size() const override { return resources_.size(); }
    void        clear() override { resources_.clear(); }
    void        visitResources(
               const std::function<void(const std::string&, const IResource&)>& visitor) const override;
    void importResource(IResourceImporter* importer) override;
    void exportResource(IResourceExporter* exporter) override;
    ///@brief 标记一次新导入的开始：清空上一轮的重复条目记录。
    ///
    ///duplicateResourceUris 只反映最近一次导入。importResource 会自动
    ///调用；进度导入路径（JarFileImporter::importResourceWithProgress）
    ///不经过本容器方法，导入方（ResourceHelper）须先调用本方法。
    void beginImport() { duplicateResourceUris_.clear(); }
    ///@brief 获取底层资源映射表（可修改）。
    std::unordered_map<std::string, std::unique_ptr<IResource>>& getResources()
    {
        return resources_;
    }

    ///@brief 导入期间重复出现的条目名，按出现顺序记录（issue ）。
    ///
    ///重复条目按 JVM 类加载语义 first-wins 保留首条目，后条目丢弃并记录
    ///在此。此前 last-wins 导入会让「工具分析的对象」与「JVM 实际加载的
    ///对象」错配。导入方应检查此列表并向用户告警。每次导入开始时清空
    ///（见 beginImport）。
    [[nodiscard]] const std::vector<std::string>& duplicateResourceUris() const noexcept
    {
        return duplicateResourceUris_;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<IResource>> resources_;
    std::vector<std::string>                                     duplicateResourceUris_;
};

}   //namespace mjt::base
