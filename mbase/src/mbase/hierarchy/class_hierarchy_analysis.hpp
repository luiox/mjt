#pragma once

#include "mbase/hierarchy/class_hierarchy.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

struct ClassHierarchyAnalysis
{
    using Result = ClassHierarchy;

    Result run(ResourceContainer& container, GlobalAnalysisManager& gam)
    {
        ClassHierarchy h;
        h.build(container, gam.arena());
        return h;
    }
};

}   //namespace mjt::base
