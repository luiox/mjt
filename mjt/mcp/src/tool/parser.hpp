#pragma once

#include <string>
#include <vector>

#include "service.hpp"

namespace mjt::mcp::detail {

bool parseSearchOptions(const Json& arguments,
                        mjbase::workspace::SearchOptions& output,
                        std::string& error);
mjbase::workspace::MutationRequest mutationRequest(const Json& arguments);
bool parseMutation(const Json& arguments,
                   mjbase::workspace::ClassMutation& mutation,
                   std::string& error);
bool parseRename(const Json& arguments,
                 mjbase::workspace::RenameRequest& output,
                 std::string& error);
bool parsePipelineRequest(const Json& arguments,
                          mjbase::workspace::PipelineRequest& output,
                          std::string& error);
bool parseWorkflowReferences(
    const Json& arguments,
    std::vector<mjbase::workspace::WorkflowReferenceQuery>& output,
    std::string& error);
bool parseWorkflowDecompileClasses(const Json& arguments,
                                   std::vector<std::string>& output,
                                   bool& batch,
                                   std::string& error);
bool parseJarRepairRequest(const Json& arguments,
                           mjbase::workspace::JarRepairRequest& output,
                           std::string& error);

}   //namespace mjt::mcp::detail
