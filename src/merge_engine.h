#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "clock_aligner.h"
#include "event_types.h"
#include "trace_writer.h"

// Merges perf events and VizTracer events into a single Chrome Trace output.
//
// Strategy:
// 1. Read all perf events into memory (sorted by timestamp)
// 2. Build scheduler and GIL state machines to compute durations
// 3. Stream VizTracer events and merge-sort with perf events
// 4. Write output incrementally

struct MergeOptions {
    bool include_sched = true;
    bool include_gil = true;
    bool verbose = false;
    int32_t filter_pid = -1;  // -1 = no filter
};

class MergeEngine {
public:
    using Options = MergeOptions;

    MergeEngine(TraceWriter &writer, ClockAligner &aligner, Options opts = Options{});

    // Add perf events (call before merge)
    void add_perf_events(std::vector<PerfEvent> events,
                         const std::unordered_map<int32_t, std::string> &comm_map);

    // Run the merge, reading VizTracer events from the callback
    void merge_viz_events(const std::vector<VizEvent> &viz_events);

    // Write just perf events (no viz)
    void write_perf_only();

    // Write just viz events (no perf)
    void write_viz_only(const std::vector<VizEvent> &viz_events);

    uint64_t perf_events_written() const { return perf_written_; }
    uint64_t viz_events_written() const { return viz_written_; }

private:
    TraceWriter &writer_;
    ClockAligner &aligner_;
    Options opts_;

    std::vector<PerfEvent> perf_events_;
    std::unordered_map<int32_t, std::string> comm_map_;

    // Scheduler state: per-thread last switch-in time
    std::unordered_map<int32_t, uint64_t> sched_switch_in_;

    // GIL state: per-thread take_gil start time
    std::unordered_map<int32_t, uint64_t> gil_start_;

    uint64_t perf_written_ = 0;
    uint64_t viz_written_ = 0;

    // Process a single perf event, potentially emitting output events
    void emit_perf_event(const PerfEvent &event);

    // Write process/thread name metadata events
    void write_metadata();

    // Check if event passes PID filter
    bool passes_filter(int32_t pid) const;
};
