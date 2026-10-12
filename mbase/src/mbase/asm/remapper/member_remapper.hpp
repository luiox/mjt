#pragma once

#include <string>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/asm/remapper/member_remap_plan.hpp"

namespace mjt::base {

class AliasAnalysis;

///@brief Surfaces a remap operation is allowed to rewrite.
///
///The default rewrites bytecode structures only. Metadata, reflection strings and resources
///require explicit opt-in.
struct RemapPolicy
{
    bool bytecode          = true;
    bool reflectionStrings = false;
    bool annotations       = false;
    bool signatures        = false;
    bool resources         = false;
    ///resources 开启时的细分门（luiox/Q6：obf 改写面显式化）。
    ///默认 true 保持既有 resources=true 的完整改写面不变；false 仅关掉对应
    ///资源面，字节码/反射常量/注解/签名/授权文本资源不受影响。
    ///MANIFEST.MF 的 Main-Class 等 class 形态主属性改写。
    bool manifest = true;
    ///META-INF/services 的 URI 换名与 provider 行内容改写。
    bool serviceLoader = true;
    ///Exact unknown-resource URIs that may be treated as mapping-like text.
    ///Empty by default: arbitrary application text must not be guessed.
    std::vector<std::string> authorizedTextResourceUris;

    static RemapPolicy bytecodeOnly();
    static RemapPolicy bytecodeAndReflectionStrings();
    static RemapPolicy bytecodeAndMetadata();
    static RemapPolicy all();
};

///@brief Apply a remap plan to one mutable class tree.
bool applyMemberRemapPlan(jbytecode::tree::ClassNode& classNode,
                          ca::str::Utf8StringArena& arena,
                          const MemberRemapPlan& plan);

///@brief Apply a remap plan to one class tree with an explicit surface policy.
bool applyMemberRemapPlan(jbytecode::tree::ClassNode& classNode,
                          ca::str::Utf8StringArena& arena,
                          const MemberRemapPlan& plan,
                          const RemapPolicy& policy);

///@brief Apply a remap plan with reflection aliases supplied by container analysis.
bool applyMemberRemapPlan(jbytecode::tree::ClassNode& classNode,
                          ca::str::Utf8StringArena& arena,
                          const MemberRemapPlan& plan,
                          const RemapPolicy& policy,
                          const AliasAnalysis* aliases);

}   //namespace mjt::base
