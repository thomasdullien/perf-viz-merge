#include "merge_engine.h"

#include <algorithm>
#include <cstring>
#include <fmt/format.h>

MergeEngine::MergeEngine(TraceWriter &writer, ClockAligner &aligner, Options opts)
    : writer_(writer), aligner_(aligner), opts_(std::move(opts)) {}

bool MergeEngine::passes_filter(int32_t pid) const {
    return opts_.filter_pid < 0 || pid == opts_.filter_pid;
}

void MergeEngine::add_perf_events(
    std::vector<PerfEvent> events,
    const std::unordered_map<int32_t, std::string> &comm_map) {

    perf_events_ = std::move(events);
    comm_map_ = comm_map;

    // Sort by timestamp
    std::sort(perf_events_.begin(), perf_events_.end(),
              [](const PerfEvent &a, const PerfEvent &b) {
                  return a.timestamp_ns < b.timestamp_ns;
              });

    if (opts_.verbose) {
        fmt::print(stderr, "Loaded {} perf events\n", perf_events_.size());
    }
}

void MergeEngine::write_metadata() {
    for (const auto &[tid, name] : comm_map_) {
        std::string args = fmt::format(R"({{"name":"{}"}})", name);
        writer_.write_metadata("thread_name", tid, tid, args);
    }
}

void MergeEngine::emit_perf_event(const PerfEvent &event) {
    double ts_us = aligner_.align_perf(event.timestamp_ns);

    switch (event.type) {
    case PerfEventType::SchedSwitch: {
        if (!opts_.include_sched) return;

        int32_t prev_tid = event.data.sched_switch.prev_tid;
        int32_t next_tid = event.data.sched_switch.next_tid;

        if (passes_filter(event.data.sched_switch.prev_pid) ||
            passes_filter(event.data.sched_switch.next_pid)) {

            // Emit "on-cpu" duration event for prev_tid if we have its switch-in time
            auto it = sched_switch_in_.find(prev_tid);
            if (it != sched_switch_in_.end()) {
                double start_us = aligner_.align_perf(it->second);
                double dur_us = ts_us - start_us;
                if (dur_us > 0 && passes_filter(event.data.sched_switch.prev_pid)) {
                    std::string args = fmt::format(
                        R"({{"cpu":{},"prev_state":{}}})",
                        event.cpu, event.data.sched_switch.prev_state);

                    std::string name = "on-cpu";
                    auto comm_it = comm_map_.find(prev_tid);
                    if (comm_it != comm_map_.end()) {
                        name = comm_it->second + " [on-cpu]";
                    }

                    writer_.write_complete(
                        name, "sched", start_us, dur_us,
                        event.data.sched_switch.prev_pid, prev_tid, args);
                    perf_written_++;
                }
                sched_switch_in_.erase(it);
            }

            // Always emit the raw sched_switch as an instant event
            {
                std::string prev_comm(event.data.sched_switch.prev_comm);
                std::string next_comm(event.data.sched_switch.next_comm);
                std::string args = fmt::format(
                    R"({{"prev_comm":"{}","prev_tid":{},"prev_state":{},"next_comm":"{}","next_tid":{},"cpu":{}}})",
                    prev_comm, prev_tid, event.data.sched_switch.prev_state,
                    next_comm, next_tid, event.cpu);

                std::string name = fmt::format("sched_switch: {} -> {}",
                                               prev_comm, next_comm);

                writer_.write_instant(name, "sched", ts_us,
                                      event.pid, event.tid, "t", args);
                perf_written_++;
            }

            // Record that next_tid is now on-cpu
            sched_switch_in_[next_tid] = event.timestamp_ns;
        }
        break;
    }

    case PerfEventType::SchedWakeup: {
        if (!opts_.include_sched) return;
        if (!passes_filter(event.data.wakeup.target_pid)) return;

        std::string args = fmt::format(
            R"({{"target_tid":{}}})", event.data.wakeup.target_tid);

        writer_.write_instant("sched_wakeup", "sched", ts_us,
                              event.pid, event.tid, "t", args);
        perf_written_++;
        break;
    }

    case PerfEventType::SchedFork: {
        if (!opts_.include_sched) return;
        if (!passes_filter(event.pid)) return;

        std::string args = fmt::format(
            R"({{"child_pid":{},"child_tid":{}}})",
            event.data.fork.child_pid, event.data.fork.child_tid);

        writer_.write_instant("process_fork", "sched", ts_us,
                              event.pid, event.tid, "t", args);
        perf_written_++;
        break;
    }

    case PerfEventType::TakeGil: {
        if (!opts_.include_gil) return;
        if (!passes_filter(event.pid)) return;

        gil_start_[event.tid] = event.timestamp_ns;
        writer_.write_begin("GIL acquire", "gil", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::TakeGilReturn: {
        if (!opts_.include_gil) return;
        if (!passes_filter(event.pid)) return;

        auto it = gil_start_.find(event.tid);
        if (it != gil_start_.end()) {
            // We already emitted B, so just emit E
            writer_.write_end("GIL acquire", "gil", ts_us,
                              event.pid, event.tid);
            perf_written_++;
            gil_start_.erase(it);
        } else {
            // No matching take_gil, emit as instant
            writer_.write_instant("GIL acquired", "gil", ts_us,
                                  event.pid, event.tid);
            perf_written_++;
        }
        break;
    }

    case PerfEventType::DropGil: {
        if (!opts_.include_gil) return;
        if (!passes_filter(event.pid)) return;

        writer_.write_instant("GIL release", "gil", ts_us,
                              event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaLaunch: {
        if (!passes_filter(event.pid)) return;
        writer_.write_instant("cuLaunchKernel", "gpu", ts_us,
                              event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaSyncStart: {
        if (!passes_filter(event.pid)) return;
        writer_.write_begin("cuStreamSynchronize", "gpu", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaSyncEnd: {
        if (!passes_filter(event.pid)) return;
        writer_.write_end("cuStreamSynchronize", "gpu", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    default:
        break;
    }
}

void MergeEngine::merge_viz_events(const std::vector<VizEvent> &viz_events) {
    write_metadata();

    // Two-pointer merge of time-sorted perf events and viz events
    size_t pi = 0; // perf index
    size_t vi = 0; // viz index

    while (pi < perf_events_.size() || vi < viz_events.size()) {
        bool emit_perf = false;

        if (pi >= perf_events_.size()) {
            emit_perf = false; // Only viz left
        } else if (vi >= viz_events.size()) {
            emit_perf = true; // Only perf left
        } else {
            // Compare timestamps (convert both to microseconds)
            double perf_us = aligner_.align_perf(perf_events_[pi].timestamp_ns);
            double viz_us = aligner_.align_viz(viz_events[vi].ts_us);
            emit_perf = (perf_us <= viz_us);
        }

        if (emit_perf) {
            emit_perf_event(perf_events_[pi]);
            pi++;
        } else {
            const VizEvent &ve = viz_events[vi];
            double ts = aligner_.align_viz(ve.ts_us);

            if (passes_filter(static_cast<int32_t>(ve.pid))) {
                std::string_view args = ve.args_json.empty() ? "{}" :
                    std::string_view(ve.args_json);
                std::string_view cat = ve.cat.empty() ? "python" :
                    std::string_view(ve.cat);

                writer_.write_viz_event(ve.ph, ve.name, cat,
                                        ts, ve.dur_us, ve.pid, ve.tid, args);
                viz_written_++;
            }
            vi++;
        }
    }

    if (opts_.verbose) {
        fmt::print(stderr, "Wrote {} perf events and {} viz events\n",
                   perf_written_, viz_written_);
    }
}

void MergeEngine::write_perf_only() {
    write_metadata();
    for (const auto &event : perf_events_) {
        emit_perf_event(event);
    }
    if (opts_.verbose) {
        fmt::print(stderr, "Wrote {} perf events\n", perf_written_);
    }
}

void MergeEngine::write_viz_only(const std::vector<VizEvent> &viz_events) {
    for (const auto &ve : viz_events) {
        double ts = aligner_.align_viz(ve.ts_us);
        if (passes_filter(static_cast<int32_t>(ve.pid))) {
            std::string_view args = ve.args_json.empty() ? "{}" :
                std::string_view(ve.args_json);
            std::string_view cat = ve.cat.empty() ? "python" :
                std::string_view(ve.cat);

            writer_.write_viz_event(ve.ph, ve.name, cat,
                                    ts, ve.dur_us, ve.pid, ve.tid, args);
            viz_written_++;
        }
    }
    if (opts_.verbose) {
        fmt::print(stderr, "Wrote {} viz events\n", viz_written_);
    }
}
