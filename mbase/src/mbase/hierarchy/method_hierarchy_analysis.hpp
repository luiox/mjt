#pragma once

#include "mbase/hierarchy/method_hierarchy.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

struct MethodHierarchyAnalysis
{
    using Result = MethodHierarchy;

    Result run(ResourceContainer& container, GlobalAnalysisManager& gam)
    {
        auto&           ch = gam.getResult<ClassHierarchyAnalysis>(container);
        MethodHierarchy mh;
        mh.build(ch);
        return mh;
    }

    ///依赖 ClassHierarchyAnalysis：类层次被失效则方法层次连锁失效（传递失效）。
    static bool dependencyInvalidated(ResourceContainer& container, const PreservedAnalyses& pa,
                                      GlobalAnalysisManager::Invalidator& inv)
    {
        return inv.invalidate<ClassHierarchyAnalysis>(container, pa);
    }
};

}   //namespace mjt::base
