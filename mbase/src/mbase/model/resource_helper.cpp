#include "resource_helper.hpp"
#include "resource_io.hpp"

namespace mjt::base {

//从 class 文件导入：创建导入器 → 委托容器导入
void ResourceHelper::importFromClassFile(ResourceContainer& container, const std::string& filePath)
{
    SimpleClassFileImporter importer(filePath);
    container.importResource(&importer);
}

//从 jar 导入：默认使用 JvmJarReader（兼容 data descriptor），支持自定义读取器
void ResourceHelper::importFromJar(ResourceContainer& container, const std::string& filePath,
                                   std::unique_ptr<IJarReader> reader)
{
    if (!reader) reader = std::make_unique<JvmJarReader>(filePath);
    JarFileImporter importer(*reader);
    container.importResource(&importer);
}

//族解码注入面：自建 JvmJarReader 并注入条目名宽容解码回调（空 = 原始
//字节直通零漂移）。与 reader 重载分离：decoder（std::function）不与 reader
//（unique_ptr）混排默认参，避免既有 3 参 reader 调用点变义。
void ResourceHelper::importFromJar(ResourceContainer& container, const std::string& filePath,
                                   const IJarReader::EntryNameDecoder& decoder)
{
    auto reader = std::make_unique<JvmJarReader>(filePath);
    if (decoder) reader->setEntryNameDecoder(decoder);
    importFromJar(container, filePath, std::move(reader));
}

//带进度的 jar 导入：经 readWithProgress 单次解析，每个 entry 回调 progress。
void ResourceHelper::importFromJar(ResourceContainer& container, const std::string& filePath,
                                   const ProgressCallback&     progress,
                                   std::unique_ptr<IJarReader> reader)
{
    if (!reader) reader = std::make_unique<JvmJarReader>(filePath);
    JarFileImporter importer(*reader);
    //进度路径不经 ResourceContainer::importResource，须手动开始一轮新导入，
    //否则 duplicateResourceUris 会累积上一轮的陈旧记录。
    container.beginImport();
    importer.importResourceWithProgress(container, progress);
}

//仅解析条目元数据（不解压），委托 jar_reader 的 listJarEntries。
std::vector<JarEntryInfo> ResourceHelper::listJarEntries(const std::string& path)
{
    return mjt::base::listJarEntries(path);
}

//导出到 jar：默认使用 JarWriter，支持自定义写入器
void ResourceHelper::exportToJar(ResourceContainer& container, const std::string& filePath,
                                 std::unique_ptr<IJarWriter> writer)
{
    if (!writer) writer = std::make_unique<JarWriter>(filePath);
    JarFileExporter exporter(*writer);
    container.exportResource(&exporter);
}

//导出单个 class 文件到磁盘
void ResourceHelper::exportToClassFile(ResourceContainer& container, const std::string& filePath,
                                       const std::string& resourcePath)
{
    SimpleClassFileExporter exporter(resourcePath, filePath);
    container.exportResource(&exporter);
}

}   //namespace mjt::base
