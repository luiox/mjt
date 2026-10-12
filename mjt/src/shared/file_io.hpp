#pragma once

//file_io.hpp
//
//mjt 共享的小文件读取助手。此前 readTextFile 在 cmd_mapping / jar_remapper
//各有一份逐字拷贝、readWholeFile 在 xref_cache 还有第三份变体，统一收敛到
//这里；实现保持与原三份完全一致（binary 模式、失败语义不变）。

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

namespace mjt {

///@brief 读取文本文件全部内容（binary 模式，不做任何转码）。
///@param path 文件路径
///@param out 输出（成功时覆盖为文件内容）
///@return false 表示文件不存在或不可读（out 保持原值）
inline bool readTextFile(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

///@brief readWholeFile 允许读取的最大文件尺寸（256 MiB）。
///
///输入可能来自不可信路径（外来 JAR 旁的 cache/数据文件），超过上限直接按
///读取失败处理，避免把畸形超大文件整体读进内存。
inline constexpr std::streamsize kMaxWholeFileBytes = 256 * 1024 * 1024;

///@brief 读取文件全部字节。文件不存在、空文件、超过 256 MiB 或读取失败返回空 vector。
///
///与既有 xref_cache 语义一致：调用方以"空值"判定不可用。
inline std::vector<ca::u8> readWholeFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize size = in.tellg();
    if (size <= 0) return {};
    if (size > kMaxWholeFileBytes) return {};
    in.seekg(0);
    std::vector<ca::u8> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    //读满判定只能用 gcount：恰好读满时 eofbit 可能置位使 good 为 false，
    //而 gcount 与期望长度相等才能同时排除截断读。
    if (static_cast<std::streamsize>(in.gcount()) != size) return {};
    return bytes;
}

}   //namespace mjt
