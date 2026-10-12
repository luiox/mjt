#pragma once

#include "mbase/hierarchy/field_hierarchy.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

struct FieldHierarchyAnalysis
{
    using Result = FieldHierarchy;

    Result run(ResourceContainer& container, GlobalAnalysisManager& gam)
    {
        auto&          ch = gam.getResult<ClassHierarchyAnalysis>(container);
        FieldHierarchy fh;
        fh.build(ch);
        return fh;
    }

    ///依赖 ClassHierarchyAnalysis：类层次被失效则字段层次连锁失效（传递失效）。
    static bool dependencyInvalidated(ResourceContainer& container, const PreservedAnalyses& pa,
                                      GlobalAnalysisManager::Invalidator& inv)
    {
        return inv.invalidate<ClassHierarchyAnalysis>(container, pa);
    }
};

}   //namespace mjt::base
