#pragma once

#include "mbase/reference/reference_index.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

///@brief Global analysis wrapper for ReferenceIndex.
///
///Passes can request this from GlobalAnalysisManager when they need direct
///method/field/type reference facts without owning the scan themselves.
struct ReferenceIndexAnalysis
{
    using Result = ReferenceIndex;

    Result run(ResourceContainer& container, GlobalAnalysisManager& gam)
    {
        ReferenceIndex index;
        index.build(container, gam.arena());
        return index;
    }
};

}   //namespace mjt::base
