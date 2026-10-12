#pragma once

#include <string>
#include <vector>
#include <array>
#include <string_view>

#include "libca/core/datatype.hpp"
#include "mbase/jar/jar_util.hpp"
#include "mbase/jar/manifest.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

///资源类型枚举。
enum class ResourceType
{
    Class,    ///class 文件
    Manifest, ///MANIFEST.MF 文件
    Jar,      ///jar 包
    Unknown,  ///其他未知类型
    SIZE
};

///@brief 将 ResourceType 枚举转换为字符串。
static inline std::string_view to_string(ResourceType type)
{
    constexpr std::array<std::string_view, static_cast<size_t>(ResourceType::SIZE)> ColorNames = {
        "Class", "Manifest", "Jar", "Unknown"};
    return ColorNames[static_cast<size_t>(type)];
}

///@brief 将字符串解析为 ResourceType 枚举。
static inline ResourceType resource_type_from_string(const std::string& str)
{
    if (str == "Class") {
        return ResourceType::Class;
    }
    else if (str == "Manifest") {
        return ResourceType::Manifest;
    }
    else if (str == "Jar") {
        return ResourceType::Jar;
    }
    else {
        return ResourceType::Unknown;
    }
}

///资源对象通用接口。
class IResource
{
public:
    virtual ~IResource() = default;
    ///@brief 获取资源 URI（jar 内路径或磁盘路径）。
    virtual std::string getLocation() const = 0;
    ///@brief 获取资源类型。
    virtual ResourceType getType() const { return ResourceType::Unknown; }
};

///@brief class 文件资源。
class ClassResource : public IResource
{
public:
    ///@brief 构造，指定路径和字节内容。
    ClassResource(std::string path, std::vector<ca::u8> content)
        : path_(std::move(path))
        , content_(std::move(content))
    {}
    std::string  getLocation() const override { return path_; }
    ResourceType getType() const override { return ResourceType::Class; }
    ///@brief 获取 class 字节数据。
    const std::vector<ca::u8>& get() const { return content_; }
    ///@brief 替换 class 字节数据。
    void set(std::vector<ca::u8> bytes) { content_ = std::move(bytes); }

private:
    std::string         path_;
    std::vector<ca::u8> content_;
};

///@brief MANIFEST.MF 资源。
class ManifestResource : public IResource
{
public:
    ///@brief 构造，封装 Manifest 对象。
    explicit ManifestResource(Manifest manifest)
        : manifest_(std::move(manifest))
    {}
    ///@brief 从字节数据构造 ManifestResource。
    static ManifestResource fromBytes(const std::vector<ca::u8>& bytes)
    {
        return ManifestResource(Manifest::fromBytes(bytes));
    }
    std::string  getLocation() const override { return JarUtil::kManifestFileName; }
    ResourceType getType() const override { return ResourceType::Manifest; }
    ///@brief 获取 Manifest 对象引用。
    const Manifest& get() const { return manifest_; }
    ///@brief 替换 Manifest 数据。
    void set(Manifest manifest) { manifest_ = std::move(manifest); }

private:
    Manifest manifest_;
};

///@brief 未知类型资源。
class UnknownResource : public IResource
{
public:
    ///@brief 构造，指定路径和内容。
    UnknownResource(std::string path, std::vector<ca::u8> content)
        : path_(std::move(path))
        , content_(std::move(content))
    {}
    std::string getLocation() const override { return path_; }
    ///@brief 获取原始字节数据。
    const std::vector<ca::u8>& get() const { return content_; }
    ///@brief 替换原始字节数据。
    void set(std::vector<ca::u8> bytes) { content_ = std::move(bytes); }

private:
    std::string         path_;
    std::vector<ca::u8> content_;
};

}   //namespace mjt::base

namespace ca::core::tag_cast {

template<>
struct TypeOf<mjt::base::ClassResource>
{
    static constexpr auto value = mjt::base::ResourceType::Class;
};

template<>
struct TypeOf<mjt::base::ManifestResource>
{
    static constexpr auto value = mjt::base::ResourceType::Manifest;
};

template<>
struct TypeOf<mjt::base::UnknownResource>
{
    static constexpr auto value = mjt::base::ResourceType::Unknown;
};
}   //namespace ca::core::tag_cast
