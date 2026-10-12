#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "mbase/jar/jar_reader.hpp"
#include "mbase/jar/jar_writer.hpp"
#include "resource_model.hpp"
#include "resource_container.hpp"

namespace mjt::base {

//── 导入导出接口 ──────────────────────────────────────────────

///@brief 资源导出器接口。
class IResourceExporter
{
public:
    virtual ~IResourceExporter() = default;
    ///@brief 将容器中所有资源导出。
    virtual void exportResource(IResourceContainer& container) = 0;
};

///@brief 资源导入器接口。
class IResourceImporter
{
public:
    virtual ~IResourceImporter() = default;
    ///@brief 从外部源导入资源到容器。
    virtual void importResource(IResourceContainer& container) = 0;
};

//── Jar 导入导出 ──────────────────────────────────────────────

///@brief 将 jar 包内容导入到资源容器。
class JarFileImporter : public IResourceImporter
{
public:
    ///loading 进度回调：(index 1-based, total, entry 名)。供进度反馈用。
    using ProgressCallback = std::function<void(int, int, const std::string&)>;

    ///@brief 构造，指定 jar 读取器。
    explicit JarFileImporter(IJarReader& reader)
        : reader_(&reader)
    {}
    void importResource(IResourceContainer& container) override;

    ///@brief 带进度的导入：经 readWithProgress 读条目，每个条目触发 progress 回调。
    ///
    ///@param container 目标容器
    ///@param progress 进度回调（每个 entry 一次，index/total/name）
    void importResourceWithProgress(IResourceContainer&     container,
                                    const ProgressCallback& progress);

private:
    IJarReader* reader_;
};

///@brief 将资源容器中的资源写入 jar 包。
class JarFileExporter : public IResourceExporter
{
public:
    ///@brief 构造，指定 jar 写入器。
    explicit JarFileExporter(IJarWriter& writer)
        : writer_(&writer)
    {}
    void exportResource(IResourceContainer& container) override;

private:
    IJarWriter* writer_;
};

//── Class 文件导入导出 ────────────────────────────────────────

///@brief 将单个 class 文件导出到磁盘。
class SimpleClassFileExporter : public IResourceExporter
{
public:
    ///@brief 构造，指定容器内资源 URI 和目标磁盘路径。
    SimpleClassFileExporter(std::string uri, std::string path);
    void exportResource(IResourceContainer& container) override;

private:
    std::string uri_;
    std::string path_;
};

///@brief 从磁盘导入单个 class 文件。
class SimpleClassFileImporter : public IResourceImporter
{
public:
    ///@brief 构造，指定磁盘文件路径。
    explicit SimpleClassFileImporter(std::string path)
        : path_(std::move(path))
    {}
    void importResource(IResourceContainer& container) override;

private:
    std::string path_;
};

//── 压缩包工具 ────────────────────────────────────────────────

///@brief 压缩包解压工具（ZIP / tar.xz / tar）。
class ZipUtil
{
public:
    ///@brief 自动识别格式并解压到目标目录。
    static void extract(const std::string& archivePath, const std::string& targetDir);
    ///@brief 解压 ZIP 文件（委托 extract）。
    static void unzip(const std::string& zipPath, const std::string& targetDir);
    ///@brief 解压压缩包中指定的单个文件。
    static void extractFile(const std::string& path, const std::string& file,
                            const std::string& destination);
};

}   //namespace mjt::base
