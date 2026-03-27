#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clock_aligner.h"
#include "event_types.h"
#include "output_writer.h"
#include "streaming_sort.h"

// Merges perf events and VizTracer events into a single trace output.
//
// Strategy:
// 1. Accept a PerfEventIterator that yields events in sorted order
// 2. Build scheduler and GIL state machines to compute durations
// 3. Two-pointer merge with sorted VizTracer events
// 4. Write output incrementally

struct MergeOptions {
    bool include_sched = true;
    bool include_gil = true;
    bool include_gpu = true;
    bool verbose = false;
    int32_t filter_pid = -1;  // -1 = no filter
    double min_duration_us = 0;  // skip viz events shorter than this (0 = keep all)
    std::vector<std::string> filter_names;  // empty = no filter; substring match, OR'd
    double time_start_s = -1;  // relative seconds from trace start (-1 = no limit)
    double time_end_s = -1;    // relative seconds from trace start (-1 = no limit)
    bool time_end_exclusive = false;  // if true, events at exactly time_end are excluded
};

class MergeEngine {
public:
    using Options = MergeOptions;

    MergeEngine(OutputWriter &writer, ClockAligner &aligner, Options opts = Options{});

    // Set the perf event source (iterator-based, streaming)
    void set_perf_source(std::unique_ptr<PerfEventIterator> iter,
                         const std::unordered_map<int32_t, std::string> &comm_map,
                         const std::vector<PerfEvent> &fork_events,
                         uint64_t last_ts_ns);

    // Legacy: load all perf events into memory (small-file path)
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
    OutputWriter &writer_;
    ClockAligner &aligner_;
    Options opts_;

    std::unique_ptr<PerfEventIterator> perf_iter_;
    std::unordered_map<int32_t, std::string> comm_map_;
    // TID → process TGID mapping (built from fork events and perf headers)
    std::unordered_map<int32_t, int32_t> tid_to_tgid_;

    // Last perf timestamp for flushing open spans at end
    uint64_t last_perf_ts_ns_ = 0;

    // Unified scheduler state: per-thread tracking with gap-fill
    struct SchedState {
        uint64_t last_event_ns;  // timestamp of last sched event
        bool on_cpu;              // true = on-cpu, false = off-cpu
        int64_t off_cpu_reason;   // prev_state when switched out
        int32_t last_cpu;
    };
    std::unordered_map<int32_t, SchedState> sched_state_;

    // GIL state: per-thread take_gil start time (waiting to acquire)
    std::unordered_map<int32_t, uint64_t> gil_start_;
    // GIL state: per-thread take_gil_return time (holding the GIL)
    std::unordered_map<int32_t, uint64_t> gil_held_;

    // GPU/NCCL dedup: per-tid set of currently open span names
    // (to handle duplicate probes like nvidia:launch + nvidia:launch_1)
    std::unordered_map<int32_t, std::unordered_set<std::string>> gpu_open_;

    uint64_t perf_written_ = 0;
    uint64_t viz_written_ = 0;
    uint64_t progress_counter_ = 0;
    uint64_t total_viz_events_ = 0;  // set before merge for progress

    void maybe_report_progress();
    uint64_t sched_mismatch_count_ = 0;
    uint64_t sched_switch_total_ = 0;
    uint64_t sched_switch_filtered_ = 0;
    uint64_t sched_switch_no_state_ = 0;
    uint64_t sched_switch_already_off_ = 0;
    uint64_t sched_switch_off_transition_ = 0;
    double offcpu_total_us_ = 0;
    uint64_t offcpu_count_ = 0;
    uint64_t cs_same_ts_count_ = 0;

    // Process a single perf event, potentially emitting output events
    void emit_perf_event(const PerfEvent &event);

    // Flush remaining open scheduler spans at trace end
    void flush_sched_state();

    // Write process/thread name metadata events
    void write_metadata();

    // Check if event passes PID filter
    bool passes_filter(int32_t pid) const;

    // Check if a thread/process passes the name filter (substring match, OR'd)
    bool passes_name_filter(int32_t tid) const;

    // Check if a timestamp (in microseconds) passes the time range filter
    bool passes_time_filter(double ts_us) const;

    // Look up the process TGID for a TID (falls back to tid itself)
    int32_t tgid_for(int32_t tid) const;

    // Build tid_to_tgid_ map from fork events and viz events
    void build_tid_map(const std::vector<PerfEvent> &fork_events,
                       const std::vector<VizEvent> &viz_events);

    // Build name map from viz metadata events (thread_name / process_name)
    void build_viz_name_map(const std::vector<VizEvent> &viz_events);

    // Compute absolute time range boundaries from relative seconds
    void compute_time_bounds(const std::vector<VizEvent> &viz_events);

    // Map from tid -> thread/process name (from viz metadata)
    std::unordered_map<int64_t, std::string> viz_name_map_;

    // Absolute time bounds in microseconds (computed from relative seconds)
    double time_start_us_ = -1;  // -1 = no limit
    double time_end_us_ = -1;    // -1 = no limit
};
