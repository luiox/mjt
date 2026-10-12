#pragma once

#include <string>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"

namespace mjt::base {

///MANIFEST.MF 模型。
///支持从字节解析和序列化回字节，提供主属性区读写。
///
///存储必须是保序结构（vector 而非 unordered_map）：toBytes 的输出字节进入
///jar 条目并被字节级 golden 测试锁定，序列化行序必须确定且与输入一致。
///std::unordered_map 的遍历序属实现定义（std::hash 与桶增长策略各标准库
///不同），libstdc++ 与 MSVC 对同两组属性给出相反行序 → 同一管线在
///Windows/linux 产出不同的 MANIFEST.MF 字节（CI 实证：golden 字节比对仅
///MANIFEST.MF 条目失配，两行互换、总字节数相同）。行序对 JVM 语义无影响，
///但产出必须平台无关，故按插入序线性存储；属性个数量级为个位数，线性
///查找无性能影响。
class Manifest
{
public:
    Manifest() = default;

    ///从 MANIFEST.MF 格式的字节创建 Manifest 对象。
    ///@param bytes 原始字节
    static Manifest fromBytes(const std::vector<ca::u8>& bytes);

    ///序列化回 MANIFEST.MF 格式字节。
    std::vector<ca::u8> toBytes() const;

    ///获取所有主属性（插入序 = 原 MANIFEST.MF 行序；对既有 key 赋值不改变
    ///位置，仅覆写值；新 key 追加尾部）。
    const std::vector<std::pair<std::string, std::string>>& getMainAttributes() const
    {
        return mainAttributes_;
    }

    ///获取指定主属性值，不存在返回空串。
    std::string getMainAttribute(const std::string& key) const
    {
        for (const auto& [k, v] : mainAttributes_) {
            if (k == key) return v;
        }
        return "";
    }

    ///设置主属性（既有 key 原位覆写；新 key 追加尾部）。
    void setMainAttribute(const std::string& key, const std::string& value)
    {
        for (auto& [k, v] : mainAttributes_) {
            if (k == key) {
                v = value;
                return;
            }
        }
        mainAttributes_.emplace_back(key, value);
    }

private:
    ///主属性，按插入序存储（见类注释：序列化行序确定性要求）。
    std::vector<std::pair<std::string, std::string>> mainAttributes_;
};

}   //namespace mjt::base
