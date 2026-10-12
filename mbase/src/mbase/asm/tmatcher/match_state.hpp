#pragma once

#include "libca/core/any.hpp"
#include "libca/str/utf8_string.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace mjt::tmatcher {

namespace detail {

inline bool any_equals(const ca::core::Any& a, const ca::core::Any& b)
{
    if (auto* p = a.as<bool>()) {
        auto* q = b.as<bool>();
        return q && *p == *q;
    }
    if (auto* p = a.as<char>()) {
        auto* q = b.as<char>();
        return q && *p == *q;
    }
    if (auto* p = a.as<signed char>()) {
        auto* q = b.as<signed char>();
        return q && *p == *q;
    }
    if (auto* p = a.as<unsigned char>()) {
        auto* q = b.as<unsigned char>();
        return q && *p == *q;
    }
    if (auto* p = a.as<short>()) {
        auto* q = b.as<short>();
        return q && *p == *q;
    }
    if (auto* p = a.as<unsigned short>()) {
        auto* q = b.as<unsigned short>();
        return q && *p == *q;
    }
    if (auto* p = a.as<int>()) {
        auto* q = b.as<int>();
        return q && *p == *q;
    }
    if (auto* p = a.as<unsigned>()) {
        auto* q = b.as<unsigned>();
        return q && *p == *q;
    }
    if (auto* p = a.as<long>()) {
        auto* q = b.as<long>();
        return q && *p == *q;
    }
    if (auto* p = a.as<unsigned long>()) {
        auto* q = b.as<unsigned long>();
        return q && *p == *q;
    }
    if (auto* p = a.as<long long>()) {
        auto* q = b.as<long long>();
        return q && *p == *q;
    }
    if (auto* p = a.as<unsigned long long>()) {
        auto* q = b.as<unsigned long long>();
        return q && *p == *q;
    }
    if (auto* p = a.as<float>()) {
        auto* q = b.as<float>();
        return q && *p == *q;
    }
    if (auto* p = a.as<double>()) {
        auto* q = b.as<double>();
        return q && *p == *q;
    }
    if (auto* p = a.as<long double>()) {
        auto* q = b.as<long double>();
        return q && *p == *q;
    }
    if (auto* p = a.as<std::string>()) {
        auto* q = b.as<std::string>();
        return q && *p == *q;
    }
    if (auto* p = a.as<ca::str::Utf8StringRef>()) {
        auto* q = b.as<ca::str::Utf8StringRef>();
        return q && *p == *q;
    }
    return false;
}

}   //namespace detail

///不可变变量捕获表。同名绑定冲突时返回 nullopt，同值重复绑定保持不变。
///
///注意：该类型被 asm/insn/expr_node.hpp 的 ExprNode::eval 虚接口引用，
///类名/方法签名/所在文件路径属于跨模块契约，改动需同步评估 expr 模块影响。
class CaptureStore
{
    std::shared_ptr<const std::unordered_map<ca::str::Utf8StringRef, ca::core::Any>> values_;
    explicit CaptureStore(
        std::shared_ptr<const std::unordered_map<ca::str::Utf8StringRef, ca::core::Any>> values)
        : values_(std::move(values))
    {}

public:
    CaptureStore()
        : values_(empty().values_)
    {}

    static CaptureStore empty()
    {
        static auto m =
            std::make_shared<const std::unordered_map<ca::str::Utf8StringRef, ca::core::Any>>();
        return CaptureStore(m);
    }

    ca::core::Any get(ca::str::Utf8StringRef name) const
    {
        auto it = values_->find(name);
        return it != values_->end() ? it->second : ca::core::Any{};
    }

    ca::core::Any get(const char* name) const
    {
        return get(ca::str::Utf8StringRef::from_cstr(name));
    }

    //ExprCapture 的既有跨模块契约使用 std::string；此处只创建临时 UTF-8 查找视图。
    ca::core::Any get(const std::string& name) const
    {
        return get(asUtf8Ref(name));
    }

    bool contains(ca::str::Utf8StringRef name) const
    {
        return values_->find(name) != values_->end();
    }

    bool contains(const char* name) const
    {
        return contains(ca::str::Utf8StringRef::from_cstr(name));
    }

    bool contains(const std::string& name) const
    {
        return contains(asUtf8Ref(name));
    }

    std::optional<CaptureStore> bind(ca::str::Utf8StringRef name,
                                     const ca::core::Any&  value) const
    {
        auto it = values_->find(name);
        if (it != values_->end()) {
            if (!it->second.has_value() || !value.has_value()) return std::nullopt;
            return detail::any_equals(it->second, value) ? std::optional(*this) : std::nullopt;
        }
        auto new_map =
            std::make_shared<std::unordered_map<ca::str::Utf8StringRef, ca::core::Any>>(*values_);
        (*new_map)[name] = value;
        return CaptureStore(new_map);
    }

    std::optional<CaptureStore> bind(const char* name, const ca::core::Any& value) const
    {
        return bind(ca::str::Utf8StringRef::from_cstr(name), value);
    }

    std::optional<CaptureStore> merge(const CaptureStore& other) const
    {
        if (values_->empty()) return other;
        if (other.values_->empty()) return *this;
        auto new_map =
            std::make_shared<std::unordered_map<ca::str::Utf8StringRef, ca::core::Any>>(*values_);
        for (const auto& [key, val] : *other.values_) {
            auto it = new_map->find(key);
            if (it != new_map->end()) {
                if (!detail::any_equals(it->second, val)) return std::nullopt;
            }
            (*new_map)[key] = val;
        }
        return CaptureStore(new_map);
    }

private:
    static ca::str::Utf8StringRef asUtf8Ref(const std::string& value)
    {
        return ca::str::Utf8StringRef::from_data(
            reinterpret_cast<const ca::u8*>(value.data()), value.size());
    }
};

///不可变匹配状态：游标 + 捕获表快照。每个回溯分支返回独立快照。
struct MatchState
{
    int          cursor;
    CaptureStore captures;

    MatchState withCursor(int newCursor) const { return {newCursor, captures}; }

    MatchState withCaptures(const CaptureStore& newCaptures) const
    {
        return {cursor, newCaptures};
    }

    static MatchState initial(int cursor) { return {cursor, CaptureStore::empty()}; }
};

}   //namespace mjt::tmatcher
