#pragma once

#include "libca/core/any.hpp"
#include <functional>
#include <string>
#include <cstdint>

#include "jbytecode/core/type.hpp"
#include "jbytecode/jbytecode.hpp"
#include "mbase/asm/analyzer/constant_payload.hpp"

namespace mjt::base {

class ConstantValue : public jbytecode::analysis::Value
{
public:
    static const ConstantValue Null;

    ConstantValue() = default;

    ConstantValue(const jbytecode::analysis::BasicValue& type, const ca::core::Any& value)
        : type_(type)
        , value_(normalizeIntValue(type, value))
    {}

    int getSize() const override { return type_.getSize(); }

    std::string toString() const
    {
        const jbytecode::core::Type* t = type_.getType();
        if (t == nullptr) return "uninitialized";
        //:getClassName 无参兜底重载已删除;名称仅在本表达式内拷贝进
        //std::string,局部 scratch arena 寿命足够。
        ca::str::Utf8StringArena classNameScratch;
        auto        clsName = t->getClassName(classNameScratch);
        std::string typeName =
            (type_ == jbytecode::analysis::BasicValue::REFERENCE_VALUE)
                ? "a reference type"
                : std::string(reinterpret_cast<const char*>(clsName.data()), clsName.byte_length());
        if (this == &Null) return "null";
        if (!value_.has_value()) return "unknown value of " + typeName;
        return valueToString() + " (" + typeName + ")";
    }

    const ca::core::Any& getValue() const { return value_; }

    void setValue(const ca::core::Any& value) { value_ = normalizeIntValue(type_, value); }

    const jbytecode::analysis::BasicValue& getType() const { return type_; }

    bool isUninitialized() const { return type_.getType() == nullptr; }

    bool isKnown() const { return value_.has_value(); }

    bool isNull() const { return this == &Null; }

    bool isInteger() const { return type_ == jbytecode::analysis::BasicValue::INT_VALUE; }

    bool isString() const { return value_.has_value() && value_.as<std::string>() != nullptr; }

    bool isLong() const { return type_ == jbytecode::analysis::BasicValue::LONG_VALUE; }

    int getAsInteger() const
    {
        if (!value_.has_value()) return 0;
        if (auto* p = value_.as<int>()) return *p;
        if (auto* p = value_.as<int64_t>()) return static_cast<int>(*p);
        return 0;
    }

    int64_t getAsLong() const
    {
        if (!value_.has_value()) return 0;
        if (auto* p = value_.as<int64_t>()) return *p;
        if (auto* p = value_.as<int>()) return static_cast<int64_t>(*p);
        return 0;
    }

    bool equals(const ConstantValue& other) const
    {
        if (this == &other) return true;
        if (this == &Null || &other == &Null) return false;
        return valueEqual(other.value_) && type_.equals(other.type_);
    }

    bool operator==(const ConstantValue& other) const { return equals(other); }
    bool operator!=(const ConstantValue& other) const { return !equals(other); }

    ///内容哈希:与 equals/operator== 同为内容语义,相等对象必有相等哈希。
    ///
    ///哈希覆盖的状态集合与 equals 逐字段一致,仅两块:
    ///1. type_:BasicValue 背后的 Type 内容——归一化 sort
    ///(INTERNAL→OBJECT,与 Type::operator== 的归一化一致,否则
    ///Type(INTERNAL,"X") 与 Type(OBJECT,"X") 这对相等对象会异哈希)
    ///+ 描述符字节内容(经 libca 现成的 std::hash<Utf8StringRef>,
    ///其为按字节内容的 FNV-1a,与 Utf8StringRef::equals 的 memcmp
    ///内容比较对应);
    ///2. value_:ca::core::Any 载荷内容,分支顺序与 constantAnyEqual
    ///一一对应;int64 与 long long 在相等与哈希两侧统一按 int64 数值
    ///处理;float/double 相等按 operator==(-0.0 == +0.0),故哈希前把
    ///-0.0 归一为 +0.0,避免位模式哈希撕裂契约。
    ///对象地址身份不进哈希(仅 Null 单例保留身份特判,对应 equals 中
    ///对 Null 的身份特判)。
    ///
    ///死亡点:哈希经 type_.getType 解引用 Type 内容。该指针是 BasicValue
    ///持有的非拥有观察指针,指向进程级静态单例(Type::INT_TYPE 等)或
    ///作业级 arena/分析会话持有的 Type,必须活过本次 hash 调用。这与
    ///equals/toString 既有的解引用要求一致,不引入新的生命周期约束。
    size_t hash() const
    {
        if (this == &Null) return ~size_t{0};
        size_t h = 7;
        h        = combineHash(h, hashType(type_));
        return combineHash(h, hashPayload(value_));
    }

private:
    jbytecode::analysis::BasicValue type_;
    ca::core::Any                   value_;

    ///局部哈希合并(与 mbase/util/triple.hpp 同款 0x9e3779b9 方案);仅本文件用。
    static size_t combineHash(size_t seed, size_t v)
    {
        return seed ^ (v + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    }

    ///Type 内容哈希:归一化 sort(INTERNAL→OBJECT,镜像 Type::operator==)
    ///+ 描述符字节内容哈希。type 为空(uninitialized)时返回固定值。
    static size_t hashType(const jbytecode::analysis::BasicValue& type)
    {
        const jbytecode::core::Type* t = type.getType();
        if (t == nullptr) return 0;
        int sort = t->getSort();
        if (sort == jbytecode::core::Type::INTERNAL) sort = jbytecode::core::Type::OBJECT;
        return combineHash(static_cast<size_t>(sort),
                           std::hash<ca::str::Utf8StringRef>{}(t->valueRef()));
    }

    ///载荷内容哈希:分支顺序与 constantAnyEqual 一一对应;int64 与 long long
    ///统一按 int64 数值哈希(两侧相等语义本就统一);float/double 先把 -0.0
    ///归一为 +0.0(相等按 operator==,位模式哈希会把这对相等值撕裂)。
    static size_t hashPayload(const ca::core::Any& value)
    {
        if (!value.has_value()) return 0;
        if (auto* p = value.as<int>()) return std::hash<int>{}(*p);
        if (auto* p = value.as<std::int64_t>()) return std::hash<std::int64_t>{}(*p);
        if (auto* p = value.as<long long>())
            return std::hash<std::int64_t>{}(static_cast<std::int64_t>(*p));
        if (auto* p = value.as<float>()) {
            float f = (*p == 0.0f) ? 0.0f : *p;
            return std::hash<float>{}(f);
        }
        if (auto* p = value.as<double>()) {
            double d = (*p == 0.0) ? 0.0 : *p;
            return std::hash<double>{}(d);
        }
        if (auto* p = value.as<std::string>()) return std::hash<std::string>{}(*p);
        if (auto* p = value.as<bool>()) return std::hash<bool>{}(*p);
        if (auto* p = value.as<char>()) return std::hash<char>{}(*p);
        if (auto* p = value.as<short>()) return std::hash<short>{}(*p);
        ///constantAnyEqual 对未知载荷恒判不等,哈希取固定值不违契约。
        return 0;
    }

    static ca::core::Any normalizeIntValue(const jbytecode::analysis::BasicValue& type,
                                           const ca::core::Any&                   value)
    {
        if (type == jbytecode::analysis::BasicValue::INT_VALUE && value.has_value()) {
            if (auto* p = value.as<bool>()) return static_cast<int>(*p);
            if (auto* p = value.as<char>()) return static_cast<int>(*p);
            if (auto* p = value.as<signed char>()) return static_cast<int>(*p);
            if (auto* p = value.as<short>()) return static_cast<int>(*p);
            if (auto* p = value.as<unsigned short>()) return static_cast<int>(*p);
            if (auto* p = value.as<float>()) return static_cast<int>(*p);
            if (auto* p = value.as<double>()) return static_cast<int>(*p);
        }
        return value;
    }

    bool valueEqual(const ca::core::Any& other) const
    {
        return constantAnyEqual(value_, other);
    }

    std::string valueToString() const
    {
        if (!value_.has_value()) return "null";
        if (auto* p = value_.as<int>()) return std::to_string(*p);
        if (auto* p = value_.as<int64_t>()) return std::to_string(*p);
        if (auto* p = value_.as<float>()) return std::to_string(*p);
        if (auto* p = value_.as<double>()) return std::to_string(*p);
        if (auto* p = value_.as<std::string>()) return *p;
        if (auto* p = value_.as<bool>()) return *p ? "true" : "false";
        return "?";
    }
};

}   //namespace mjt::base
