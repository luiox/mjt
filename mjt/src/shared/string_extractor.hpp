#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

//── StringEntry ────────────────────────────────────────────────
//表示从常量池提取的一个字符串条目

struct StringEntry
{
    std::string className;   //来源类名
    std::string value;       //字符串值
    enum Type
    {
        Utf8,
        String,
        Class
    };
    Type type = Utf8;
};

//── StringExtractor ────────────────────────────────────────────
//解析 class 文件常量池，提取 Utf8 / String / Class 条目

struct EncryptHeuristic
{
    int  minLength      = 20;
    bool checkBase64    = true;
    bool checkHex       = true;
    bool checkInvisible = true;
};

class StringExtractor
{
public:
    ///@brief 提取 class 字节中所有字符串常量
    std::vector<StringEntry> extractAll(const std::vector<ca::u8>& classBytes,
                                        const std::string&         className);

    ///@brief 按启发式规则提取疑似密文字符串
    std::vector<StringEntry> extractEncrypted(const std::vector<ca::u8>& classBytes,
                                              const std::string&         className,
                                              const EncryptHeuristic&    h = {});

    ///@brief 与 extractAll 同口径的条目计数，不物化字符串值。
    ///
    ///供 JAR 概览等只需数量的场景使用：避免为每个条目构造 std::string
    ///与容器增长（大 JAR 数万条目时的主要开销）。解析失败返回 0。
    int countAll(const std::vector<ca::u8>& classBytes);
};
