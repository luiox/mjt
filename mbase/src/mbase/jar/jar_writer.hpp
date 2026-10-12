#pragma once

#include <functional>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

namespace mjt::base {

///JAR 条目写入器。
///由 IJarWriter::write 的回调持有，调用者通过该接口逐个写条目。
class IEntryWriter
{
public:
    virtual ~IEntryWriter() = default;
    ///写入一个条目。
    ///@param entryName 条目路径
    ///@param content 条目内容
    ///
    ///按值接收：调用方持有 lvalue 时复制一次（与 const& 语义一致），持有临时对象
    ///（如 `toBytes` 返回值）时移动零拷贝——JAR-3 收敛收集路径的冗余复制。
    virtual void writeEntry(std::string entryName, std::vector<ca::u8> content) = 0;
};

///JAR 写入接口。
class IJarWriter
{
public:
    virtual ~IJarWriter() = default;
    ///写出完整 jar。
    ///@param handler 回调，handler 会被反复调用 writeEntry 直到返回
    virtual void write(const std::function<void(IEntryWriter&)>& handler) = 0;
};

///@brief 使用 DirectZipWriter 写入 jar。
///与被删除的 JvmJarWriter 完全同名同义（后者从未做过任何 JVM 特殊处理）。
class JarWriter : public IJarWriter
{
public:
    ///@brief 构造，指定输出路径。
    explicit JarWriter(std::string path);
    void write(const std::function<void(IEntryWriter&)>& handler) override;

private:
    std::string path_;
};

}   //namespace mjt::base
