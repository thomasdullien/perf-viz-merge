#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Unified event representation used by the merge engine.
// Both perf events and VizTracer events are converted to this format.

enum class EventSource : uint8_t {
    Perf,
    VizTracer,
};

enum class PerfEventType : uint8_t {
    SchedSwitch,
    SchedWakeup,
    SchedFork,
    TakeGil,
    TakeGilReturn,
    DropGil,
    NvidiaLaunch,
    NvidiaSyncStart,
    NvidiaSyncEnd,
    Comm, // pid/tid -> name mapping
    Other,
};

// A decoded perf event, stored in memory for the merge phase.
struct PerfEvent {
    uint64_t timestamp_ns;
    int32_t pid;
    int32_t tid;
    int32_t cpu;
    PerfEventType type;

    // Type-specific data
    union {
        struct {
            int32_t prev_tid;
            int32_t next_tid;
            int32_t prev_pid;
            int32_t next_pid;
            int64_t prev_state;
            char prev_comm[17];
            char next_comm[17];
        } sched_switch;
        struct {
            int32_t target_pid;
            int32_t target_tid;
        } wakeup;
        struct {
            int32_t parent_tid;
            int32_t child_tid;
            int32_t child_pid;
        } fork;
        struct {
            char comm[17];
        } comm;
    } data;

    PerfEvent() : timestamp_ns(0), pid(0), tid(0), cpu(0),
                  type(PerfEventType::Other), data{} {}
};

// A VizTracer event, yielded one at a time from the JSON stream.
struct VizEvent {
    double ts_us;       // timestamp in microseconds
    double dur_us;      // duration (for X events), 0 otherwise
    int64_t pid;
    int64_t tid;
    char ph;            // phase: B, E, X, i, etc.
    std::string name;
    std::string cat;
    std::string args_json; // raw JSON string of args
};

// Chrome Trace output event (used by TraceWriter).
struct TraceOutputEvent {
    double ts_us;
    double dur_us;
    int64_t pid;
    int64_t tid;
    std::string_view ph;
    std::string_view name;
    std::string_view cat;
    std::string args_json;
};
