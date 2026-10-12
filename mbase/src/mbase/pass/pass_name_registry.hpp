#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mbase/spi/pass_manager.hpp"     //AsmMethodPass/AsmClassPass/AsmGlobalPass
#include "mbase/pass/pass_registry.hpp"   //PipelineBuilder

namespace mjt::base {

///单 pass 的作用域。
enum class PassScope
{
    Method,
    Class,
    Global,
};

///单 pass 的元信息（供 help / list 用）。
struct PassNameInfo
{
    std::string name;
    std::string description;
    PassScope   scope;
    PassStability stability = PassStability::OptIn;
};

///@brief 单 pass 名注册中心：name → 带 scope 标记的工厂。
///
///与 PassRegistry（流水线注册）互补：PassRegistry 注册的是命名流水线
///（default-optimize、phase.*），本注册中心注册单个 pass，供 `-p`/`--pass`
///按名字选择。applyTo 按 scope 自动调用 PipelineBuilder 的 addMethodPass/
///addClassPass/addGlobalPass。
///
///@warning 非线程安全。注册和查询应在初始化阶段完成。
class PassNameRegistry
{
public:
    using MethodFactory = std::function<std::unique_ptr<AsmMethodPass>()>;
    using ClassFactory  = std::function<std::unique_ptr<AsmClassPass>()>;
    using GlobalFactory = std::function<std::unique_ptr<AsmGlobalPass>()>;

    static PassNameRegistry& instance();

    ///注册一个方法级 pass。同名覆盖。
    PassNameRegistry& registerMethodPass(const std::string& name, const std::string& description,
                                         MethodFactory factory,
                                         PassStability stability = PassStability::OptIn);
    ///注册一个类级 pass。
    PassNameRegistry& registerClassPass(const std::string& name, const std::string& description,
                                        ClassFactory factory,
                                        PassStability stability = PassStability::OptIn);
    ///注册一个全局级 pass。
    PassNameRegistry& registerGlobalPass(const std::string& name, const std::string& description,
                                         GlobalFactory factory,
                                         PassStability stability = PassStability::OptIn);

    ///是否已注册某名字。
    bool has(const std::string& name) const;

    ///查询某名字的元信息；未注册返回 nullptr。
    const PassNameInfo* info(const std::string& name) const;

    ///列出所有已注册 pass（按 name 排序，供 `--pass list` 等稳定输出）。
    std::vector<PassNameInfo> list() const;

    ///把命名 pass 加到 builder（按 scope 自动选 addMethod/Class/GlobalPass）。
    ///未注册返回 false。
    bool applyTo(const std::string& name, PipelineBuilder& builder) const;

private:
    PassNameRegistry() = default;
    struct Entry
    {
        PassNameInfo  meta;
        MethodFactory methodFactory;
        ClassFactory  classFactory;
        GlobalFactory globalFactory;
    };
    std::unordered_map<std::string, Entry> entries_;
};

///@brief 注册内置单 pass（ConstantFolder、ArithmeticSimplifier 等）。幂等。
///
///名字用真实类名（如 "ConstantFolder" 而非 issue 示意的 "DeadCodeCleaner"），
///与 pass_registry.cpp 内 phase builder 里 new 出的类一一对应。
///
///@return 本次注册的 pass 数（已注册过返回 0）。
ca::usize registerBuiltinPasses();

}   //namespace mjt::base
