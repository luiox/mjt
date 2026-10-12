#include "jar_reader.hpp"

#include <filesystem>
#include <fstream>

#include "jzip/zip_io.hpp"

namespace mjt::base {

namespace {

//读取整个文件到内存（两个 reader 共用，避免重复样板）。
std::vector<ca::u8> readFileBytes(const std::string& path)
{
    namespace fs = std::filesystem;
    const auto nativePath = fs::u8path(path);
    if (!fs::exists(nativePath)) throw std::runtime_error("jar file not found: " + path);
    std::ifstream in(nativePath, std::ios::binary);
    if (!in) throw std::runtime_error("failed to open: " + path);
    in.seekg(0, std::ios::end);
    auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<ca::u8> bytes(size);
    if (size > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

}   //namespace

//── IJarReader 基类默认实现：转调无进度 read，index/total 传 0（未知）──
void IJarReader::readWithProgress(const ProgressConsumer& consumer)
{
    read([&](const std::string& name, const std::vector<ca::u8>& data) {
        consumer(0, 0, name, data);   //0/0 表示进度未知
    });
}

JarReader::JarReader(std::string path)
    : path_(std::move(path))
{}

//使用 standard zip 读取器解析 jar，逐条目回调 consumer
void JarReader::read(
    const std::function<void(const std::string&, const std::vector<ca::u8>&)>& consumer)
{
    auto bytes   = readFileBytes(path_);
    auto archive = jzip::ZipIO::readStandard(jzip::MemorySegment(bytes));
    for (const auto* local : archive.getLocalFiles())
        consumer(decodeEntryName(local->getFileName()), jzip::ZipIO::decompressLocalFile(*local));
}

//带进度的版本：单次解析，total 来自 getLocalFiles.size，无重复 IO。
void JarReader::readWithProgress(const ProgressConsumer& consumer)
{
    auto        bytes   = readFileBytes(path_);
    auto        archive = jzip::ZipIO::readStandard(jzip::MemorySegment(bytes));
    const auto& locals  = archive.getLocalFiles();
    const int   total   = static_cast<int>(locals.size());
    int         index   = 0;
    for (const auto* local : locals) {
        ++index;   //1-based
        consumer(index, total, decodeEntryName(local->getFileName()),
                 jzip::ZipIO::decompressLocalFile(*local));
    }
}

JvmJarReader::JvmJarReader(std::string path)
    : path_(std::move(path))
{}

//使用 JVM 兼容 zip 读取器解析 jar，逐条目回调 consumer
void JvmJarReader::read(
    const std::function<void(const std::string&, const std::vector<ca::u8>&)>& consumer)
{
    auto bytes   = readFileBytes(path_);
    auto archive = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes));
    lastReadReport_ = archive.getReadReport();
    for (const auto* local : archive.getLocalFiles())
        consumer(decodeEntryName(local->getFileName()), jzip::ZipIO::decompressLocalFile(*local));
}

void JvmJarReader::readWithProgress(const ProgressConsumer& consumer)
{
    auto        bytes   = readFileBytes(path_);
    auto        archive = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes));
    lastReadReport_     = archive.getReadReport();
    const auto& locals  = archive.getLocalFiles();
    const int   total   = static_cast<int>(locals.size());
    int         index   = 0;
    for (const auto* local : locals) {
        ++index;
        consumer(index, total, decodeEntryName(local->getFileName()),
                 jzip::ZipIO::decompressLocalFile(*local));
    }
}

//仅解析条目元数据：读文件 + 解析 ZIP 结构，不解压任何条目。local 头经 CEN 关联后
//携带权威未压缩大小（zip_model 的 adopt 语义），与 JvmJarReader 的条目顺序一致。
std::vector<JarEntryInfo> listJarEntries(const std::string&                  path,
                                         const IJarReader::EntryNameDecoder& decoder)
{
    auto        bytes   = readFileBytes(path);
    auto        archive = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes));
    const auto& locals  = archive.getLocalFiles();
    std::vector<JarEntryInfo> out;
    out.reserve(locals.size());
    for (const auto* local : locals) {
        JarEntryInfo info;
        info.name             = decoder ? decoder(local->getFileName()) : local->getFileName();
        info.uncompressedSize = static_cast<ca::u64>(local->getUncompressedSize());
        info.compressedSize   = static_cast<ca::u64>(local->getCompressedSize());
        out.push_back(std::move(info));
    }
    return out;
}

}   //namespace mjt::base
