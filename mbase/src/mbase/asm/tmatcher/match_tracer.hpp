#pragma once

#include <memory>
#include <string>
#include <vector>

namespace mjt::tmatcher {

enum class TracePhase
{
    ENTER,
    SUCCESS,
    FAIL
};

struct TraceEvent
{
    TracePhase  phase;
    std::string nodeId;
    int         depth;
    int         cursor;
    int         nextCursor;
    std::string detail;
};

class MatchTrace
{
    std::vector<TraceEvent> events_;

public:
    explicit MatchTrace(std::vector<TraceEvent> events)
        : events_(std::move(events))
    {}

    const std::vector<TraceEvent>& events() const { return events_; }

    int furthestCursor() const
    {
        int f = -1;
        for (auto& e : events_) {
            f = std::max(f, e.cursor);
            f = std::max(f, e.nextCursor);
        }
        return f;
    }

    std::string toPrettyString() const
    {
        std::string sb;
        for (auto& e : events_) {
            sb.append(static_cast<size_t>(e.depth) * 2, ' ');
            switch (e.phase) {
            case TracePhase::ENTER: sb += "ENTER "; break;
            case TracePhase::SUCCESS: sb += "OK    "; break;
            case TracePhase::FAIL: sb += "FAIL  "; break;
            }
            sb += e.nodeId;
            sb += " @" + std::to_string(e.cursor);
            if (e.phase == TracePhase::SUCCESS) {
                sb += " -> " + std::to_string(e.nextCursor);
            }
            if (!e.detail.empty()) {
                sb += " : " + e.detail;
            }
            sb += '\n';
        }
        return sb;
    }
};

class MatchTracer
{
    bool                    enabled_;
    std::vector<TraceEvent> events_;

public:
    static MatchTracer noop() { return MatchTracer(false); }
    static MatchTracer recording() { return MatchTracer(true); }

    void enter(const std::string& nodeId, int depth, int cursor, const std::string& detail = {})
    {
        if (!enabled_) return;
        events_.push_back({TracePhase::ENTER, nodeId, depth, cursor, cursor, detail});
    }

    void success(const std::string& nodeId, int depth, int cursor, int nextCursor,
                 const std::string& detail = {})
    {
        if (!enabled_) return;
        events_.push_back({TracePhase::SUCCESS, nodeId, depth, cursor, nextCursor, detail});
    }

    void fail(const std::string& nodeId, int depth, int cursor, const std::string& detail = {})
    {
        if (!enabled_) return;
        events_.push_back({TracePhase::FAIL, nodeId, depth, cursor, cursor, detail});
    }

    MatchTrace snapshot() const { return MatchTrace(events_); }

private:
    explicit MatchTracer(bool enabled)
        : enabled_(enabled)
    {}
};

}   //namespace mjt::tmatcher
