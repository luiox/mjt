#pragma once

#include <string>
#include <vector>

#include "service.hpp"

namespace mjt::mcp::detail {

bool parseSearchOptions(const Json& arguments,
                        mjt::workspace::SearchOptions& output,
                        std::string& error);
mjt::workspace::MutationRequest mutationRequest(const Json& arguments);
bool parseMutation(const Json& arguments,
                   mjt::workspace::ClassMutation& mutation,
                   std::string& error);
bool parseRename(const Json& arguments,
                 mjt::workspace::RenameRequest& output,
                 std::string& error);
bool parsePipelineRequest(const Json& arguments,
                          mjt::workspace::PipelineRequest& output,
                          std::string& error);
bool parseWorkflowReferences(
    const Json& arguments,
    std::vector<mjt::workspace::WorkflowReferenceQuery>& output,
    std::string& error);
bool parseWorkflowDecompileClasses(const Json& arguments,
                                   std::vector<std::string>& output,
                                   bool& batch,
                                   std::string& error);
bool parseJarRepairRequest(const Json& arguments,
                           mjt::workspace::JarRepairRequest& output,
                           std::string& error);

}   //namespace mjt::mcp::detail
