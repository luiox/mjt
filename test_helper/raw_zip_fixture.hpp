/// @file        raw_zip_fixture.hpp
/// @brief       原始 zip 测试语料直写：jzip::ZipArchive + LocalFileHeader 全
///              STORED 条目，条目名原样落盘（不经 JarWriter / exportToJar）。
///
/// 消费方 target 须经 mjbase 公开依赖链取到 jzip 头路径并在链接闭包含
/// libjzip（同 mjt/test/cli/cli_fixture.hpp 惯例）；header-only，无 test_helper
/// 侧编译件。
#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "jzip/zip_io.hpp"
#include "libca/zip/checksum.hpp"

namespace test {

/// 写原始 zip（条目名原样落盘）。绕开写侧路径规范化（mjbase#167 折叠/拒写）：
/// 尾斜杠目录条目、双斜杠路径等非常规形态语料必须经此直写才能原样存活到
/// 读回侧（listJarEntries / 容器导入 / 分析库 ingest 的分类面覆盖）。
/// 全 STORED 条目，读回侧无感。
inline void writeRawZip(const std::filesystem::path& path,
                        const std::vector<std::pair<std::string, std::vector<ca::u8>>>& entries)
{
    jzip::ZipArchive archive;
    for (const auto& [name, data] : entries) {
        auto local = std::make_unique<jzip::LocalFileHeader>();
        ca::zip::Crc32 crcCalc;
        crcCalc.update(data.data(), data.size());
        local->setFileName(name);
        local->setCompressionMethod(0);   // STORED
        local->setCompressedSize(static_cast<ca::u32>(data.size()));
        local->setUncompressedSize(static_cast<ca::u32>(data.size()));
        local->setFileData(data);
        local->setCrc32(crcCalc.value());
        local->setGeneralPurposeBitFlag(0x0800);   // bit11 = UTF-8
        archive.addPart(std::move(local));
    }
    const auto bytes = jzip::ZipIO::writeDirect(archive);
    std::ofstream    out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) throw std::runtime_error("failed to write raw zip fixture: " + path.string());
}

}   // namespace test
