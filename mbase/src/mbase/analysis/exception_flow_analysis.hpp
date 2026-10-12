#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "mbase/model/resource_container.hpp"
#include "mbase/reference/reference_index.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

struct ExceptionFlowEdge
{
    MethodRefKey source;
    MethodRefKey callee;
    std::string  exceptionType;
    std::string  handlerType;
    bool         handled = false;
};

struct ExceptionMethodSummary
{
    MethodRefKey             method;
    std::vector<std::string> uncaughtTypes;
};

class ExceptionFlowModel
{
public:
    void build(ResourceContainer& container, ca::str::Utf8StringArena& arena);
    void clear();

    const ExceptionMethodSummary* summaryOf(const std::string& owner, const std::string& name,
                                            const std::string& desc) const;
    const std::vector<ExceptionFlowEdge>& edges() const { return edges_; }

private:
    std::vector<ExceptionMethodSummary>        summaries_;
    std::unordered_map<std::string, ca::usize> byKey_;
    std::vector<ExceptionFlowEdge>             edges_;
};

struct ExceptionFlowAnalysis
{
    using Result = ExceptionFlowModel;
    Result run(ResourceContainer& container, GlobalAnalysisManager& gam);
};

}   //namespace mjt::base
