#include "jar_writer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include "mbase/util/dos_time_util.hpp"

#include "jzip/decompressor.hpp"
#include "jzip/zip_io.hpp"
#include "libca/zip/checksum.hpp"

namespace mjt::base {
namespace {

//收集回调中所有的条目，用于后续统一排序和写入
struct Collector : IEntryWriter
{
    std::vector<std::pair<std::string, std::vector<ca::u8>>> entries;
    void writeEntry(std::string name, std::vector<ca::u8> data) override
    {
        entries.emplace_back(std::move(name), std::move(data));   //JAR-3：按值接收，零额外拷贝
    }
};



//将路径分隔符统一为 POSIX 风格
std::string normalize_path(const std::string& name)
{
    std::string out = name;
    for (auto& ch : out) {
        if (ch == '\\') ch = '/';
    }
    return out;
}

//将收集的条目写入 jar：排序 → 生成目录条目 → 写入文件条目（DEFLATE 压缩）
void writeArchive(const std::string& path, Collector& collector)
{
    auto dosTime = current_dos_time();
    auto dosDate = current_dos_date();

    //按文件名排序，保证可重现构建
    std::sort(collector.entries.begin(), collector.entries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    jzip::ZipArchive                archive;
    std::unordered_set<std::string> dirs;

    for (auto& [name, data] : collector.entries) {
        auto entryName = normalize_path(name);

        //为每级路径创建 STORED 目录条目（仅首次遇到时）
        auto slash = entryName.find('/');
        while (slash != std::string::npos) {
            std::string dirPath = entryName.substr(0, slash + 1);
            if (dirs.insert(dirPath).second) {
                auto dirEntry = std::make_unique<jzip::LocalFileHeader>();
                dirEntry->setFileName(dirPath);
                dirEntry->setCompressionMethod(0);   //STORED
                dirEntry->setCompressedSize(0);
                dirEntry->setUncompressedSize(0);
                dirEntry->setCrc32(0);
                dirEntry->setGeneralPurposeBitFlag(0x0800);   //bit11=UTF-8
                dirEntry->setLastModFileTime(dosTime);
                dirEntry->setLastModFileDate(dosDate);
                dirEntry->setOffset(0);
                archive.addPart(std::move(dirEntry));
            }
            slash = entryName.find('/', slash + 1);
        }

        //跳过纯目录条目（已在上面处理）
        if (!entryName.empty() && entryName.back() == '/') continue;

        //计算 CRC32 并对非空数据执行 DEFLATE 压缩
        auto       local = std::make_unique<jzip::LocalFileHeader>();
        ca::zip::Crc32 crcCalc;
        crcCalc.update(data.data(), data.size());
        ca::u32 crc = crcCalc.value();

        if (data.empty()) {
            local->setCompressionMethod(0);
            local->setCompressedSize(0);
            local->setUncompressedSize(0);
        }
        else {
            auto compressed = jzip::DeflateCompressor().compress(data);
            local->setCompressionMethod(8);
            local->setCompressedSize(static_cast<ca::u32>(compressed.size()));
            local->setUncompressedSize(static_cast<ca::u32>(data.size()));
            local->setFileData(std::move(compressed));
        }

        local->setFileName(entryName);
        local->setCrc32(crc);
        local->setLastModFileTime(dosTime);
        local->setLastModFileDate(dosDate);
        local->setGeneralPurposeBitFlag(0x0800);
        local->setOffset(0);
        archive.addPart(std::move(local));
    }
    auto          outBytes = jzip::ZipIO::writeDirect(archive);
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
    if (!out) throw std::runtime_error("JarWriter: cannot open output jar: " + path);
    out.write(reinterpret_cast<const char*>(outBytes.data()),
              static_cast<std::streamsize>(outBytes.size()));
    //close 会 flush；磁盘满/断连等写失败会置 failbit/badbit，绝不能静默返回一个截断的 jar。
    out.close();
    if (!out) throw std::runtime_error("JarWriter: failed to write jar (disk full/IO error): " + path);
}

}   //namespace

//── JarWriter ────────────────────────────────────────────────

JarWriter::JarWriter(std::string path)
    : path_(std::move(path))
{}

//收集回调中的条目并写出为标准 jar
void JarWriter::write(const std::function<void(IEntryWriter&)>& handler)
{
    Collector collector;
    handler(collector);
    writeArchive(path_, collector);
}

}   //namespace mjt::base
