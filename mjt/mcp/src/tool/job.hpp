#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "service.hpp"

namespace mjt::mcp {

///@brief MCP session 内一个异步任务的共享状态。
struct JobState final
{
    std::mutex       mutex;
    std::atomic_bool cancellationRequested{false};
    std::string      id;
    std::string      projectId;
    std::string      kind;
    ca::u64          sourceRevision = 0;
    std::string      state = "queued";
    Json             result;
    bool             resultIsError = false;
    std::thread      worker;
};

///@brief 将异步任务当前状态序列化为 MCP JSON 响应。
Json jobStateJson(const std::shared_ptr<JobState>& job);

}   //namespace mjt::mcp
