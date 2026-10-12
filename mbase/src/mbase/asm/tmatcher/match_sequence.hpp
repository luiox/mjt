#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libca/str/utf8_string.hpp"

#include "mbase/asm/tmatcher/match_state.hpp"
#include "mbase/asm/tmatcher/match_step.hpp"

namespace mjt::tmatcher {

///命名匹配序列：一个带名字的匹配 step。
///一个或多个 step 组成一个 MatchSequence，作为匹配器的序列模式。
///匹配结果通过名字检索（MatchResult = {name → [MatchedArea]}）。
class MatchSequence
{
    ca::str::Utf8StringRef      name_;
    std::unique_ptr<IMatchStep> step_;

public:
    ///name 是非拥有 UTF-8 视图，调用方须保证规则存活期间其底层数据有效。
    MatchSequence(ca::str::Utf8StringRef name, std::unique_ptr<IMatchStep> step)
        : name_(name)
        , step_(std::move(step))
    {}

    MatchSequence(const char* name, std::unique_ptr<IMatchStep> step)
        : MatchSequence(ca::str::Utf8StringRef::from_cstr(name), std::move(step))
    {}

    ca::str::Utf8StringRef name() const { return name_; }
    IMatchStep&             step() const { return *step_; }
    std::unique_ptr<IMatchStep> takeStep() { return std::move(step_); }
};

///命中区域：一次匹配的 [start, end) 区间与捕获表。
///使用整数下标而非原始指令指针，匹配结果与后续 list 变更解耦。
struct MatchedArea
{
    ca::str::Utf8StringRef name;          //所属 MatchSequence 的名字
    int                    start;         //命中起点（含）
    int                    endExclusive;  //命中终点（不含）
    CaptureStore           captures;      //本次命中的捕获表
};

///匹配结果：每个 MatchSequence 名字对应其全部命中区域列表。
class MatchResult
{
    std::map<ca::str::Utf8StringRef, std::vector<MatchedArea>> areas_;

public:
    MatchResult() = default;

    void add(MatchedArea area) { areas_[area.name].push_back(std::move(area)); }

    ///取某个序列名字下的全部命中。不存在则返回空。
    const std::vector<MatchedArea>& areasOf(ca::str::Utf8StringRef name) const
    {
        static const std::vector<MatchedArea> empty;
        auto it = areas_.find(name);
        return it != areas_.end() ? it->second : empty;
    }

    const std::vector<MatchedArea>& areasOf(const char* name) const
    {
        return areasOf(ca::str::Utf8StringRef::from_cstr(name));
    }

    ///全部命中（按序列名分组）。
    const std::map<ca::str::Utf8StringRef, std::vector<MatchedArea>>& all() const
    {
        return areas_;
    }

    ///总命中数。
    size_t totalMatches() const
    {
        size_t n = 0;
        for (auto& [_, v] : areas_) n += v.size();
        return n;
    }

    bool empty() const { return areas_.empty(); }
};

}   //namespace mjt::tmatcher
