#pragma once

#include "mbase/asm/remapper/member_remapper.hpp"
#include "mbase/model/resource_container.hpp"

namespace mjt::base {

///@brief Apply a remap plan to every class and opt-in resource in a container.
///
///The container orchestrator builds reflection aliases once, delegates class-tree changes to
///MemberRemapper, and moves or rewrites resources only when the policy permits it.
bool applyResourceRemapPlan(ResourceContainer& container, ca::str::Utf8StringArena& arena,
                            const MemberRemapPlan& plan);

///@brief Apply a remap plan to a container with an explicit surface policy.
bool applyResourceRemapPlan(ResourceContainer& container, ca::str::Utf8StringArena& arena,
                            const MemberRemapPlan& plan, const RemapPolicy& policy);

}   //namespace mjt::base
