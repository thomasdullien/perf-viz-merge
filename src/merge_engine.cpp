#include "merge_engine.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <fmt/format.h>

MergeEngine::MergeEngine(OutputWriter &writer, ClockAligner &aligner, Options opts)
    : writer_(writer), aligner_(aligner), opts_(std::move(opts)) {}

bool MergeEngine::passes_filter(int32_t pid) const {
    return opts_.filter_pid < 0 || pid == opts_.filter_pid;
}

int32_t MergeEngine::tgid_for(int32_t tid) const {
    auto it = tid_to_tgid_.find(tid);
    return it != tid_to_tgid_.end() ? it->second : tid;
}

void MergeEngine::build_tid_map(const std::vector<VizEvent> &viz_events) {
    // From perf fork events: child_tid belongs to child_pid (which IS the tgid)
    for (const auto &event : perf_events_) {
        if (event.type == PerfEventType::SchedFork) {
            int32_t child_pid = event.data.fork.child_pid;
            int32_t child_tid = event.data.fork.child_tid;
            if (child_pid > 0 && child_tid > 0) {
                tid_to_tgid_[child_tid] = child_pid;
            }
        }
        // The perf event header pid is the real TGID
        if (event.pid > 0 && event.tid > 0) {
            tid_to_tgid_[event.tid] = event.pid;
        }
    }
    // From VizTracer: pid is always the real process PID
    for (const auto &ve : viz_events) {
        if (ve.pid > 0 && ve.tid > 0) {
            tid_to_tgid_[static_cast<int32_t>(ve.tid)] = static_cast<int32_t>(ve.pid);
        }
    }
    if (opts_.verbose) {
        fmt::print(stderr, "Built TID->TGID map with {} entries\n", tid_to_tgid_.size());
    }
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

// Clean up VizTracer THREAD_MAP instant event names.
// Input:  "THREAD_MAP: local_tid=476551 global_tid=476551 name='mcproc__0'"
// Output: "476551 mcproc__0"  (if local==global)
// Output: "476551/476551 mcproc__0"  (if local!=global, shouldn't happen)
static std::string clean_thread_map_name(std::string_view name) {
    // Parse: "THREAD_MAP: local_tid=N global_tid=M name='...'"
    auto local_pos = name.find("local_tid=");
    auto global_pos = name.find("global_tid=");
    auto name_pos = name.find("name='");
    if (local_pos == std::string_view::npos ||
        global_pos == std::string_view::npos ||
        name_pos == std::string_view::npos)
        return std::string(name);

    auto local_val = name.substr(local_pos + 10);
    auto local_end = local_val.find(' ');
    auto local_tid = local_val.substr(0, local_end);

    auto global_val = name.substr(global_pos + 11);
    auto global_end = global_val.find(' ');
    auto global_tid = global_val.substr(0, global_end);

    auto thread_name = name.substr(name_pos + 6);
    auto quote_end = thread_name.find('\'');
    if (quote_end != std::string_view::npos)
        thread_name = thread_name.substr(0, quote_end);

    if (local_tid == global_tid) {
        return fmt::format("{} {}", local_tid, thread_name);
    }
    return fmt::format("{}/{} {}", local_tid, global_tid, thread_name);
}

static const char *prev_state_name(int64_t state) {
    switch (state & 0x0f) {
    case 0:  return "Running (preempted)";
    case 1:  return "Sleeping (interruptible)";
    case 2:  return "Disk sleep (uninterruptible)";
    case 4:  return "Stopped";
    case 8:  return "Traced";
    default: return "Unknown";
    }
}

void MergeEngine::write_metadata() {
    // Collect unique PIDs and their process names
    std::unordered_map<int32_t, std::string> pid_names;
    // Track which TIDs we've seen for GIL/sched synthetic track metadata
    std::unordered_set<int32_t> seen_tids;

    for (const auto &[tid, name] : comm_map_) {
        int32_t tgid = tgid_for(tid);
        std::string args = fmt::format(R"({{"name":"{}"}})", name);
        writer_.write_metadata("thread_name", tgid, tid, args);
        seen_tids.insert(tid);

        // Track process names by TGID
        if (pid_names.find(tgid) == pid_names.end()) {
            pid_names[tgid] = name;
        }
    }
    // Also derive process names from perf events
    for (const auto &event : perf_events_) {
        if (event.pid > 0 && pid_names.find(event.pid) == pid_names.end()) {
            auto it = comm_map_.find(event.pid);
            if (it != comm_map_.end()) {
                pid_names[event.pid] = it->second;
            }
        }
    }
    for (const auto &[pid, name] : pid_names) {
        std::string args = fmt::format(R"({{"name":"{}"}})", name);
        writer_.write_metadata("process_name", pid, 0, args);
    }

    // Emit metadata for synthetic GIL and sched tracks, with sort order:
    // sort_index 0 = sched track, 1 = GIL track, 2 = call stacks (default)
    for (int32_t tid : seen_tids) {
        auto comm_it = comm_map_.find(tid);
        std::string base = comm_it != comm_map_.end() ? comm_it->second : std::to_string(tid);
        int32_t tgid = tgid_for(tid);

        // Sort index for the real thread (call stacks)
        writer_.write_metadata("thread_sort_index", tgid, tid,
                               R"({"sort_index":2})");

        if (opts_.include_sched) {
            int64_t sched_tid = static_cast<int64_t>(tid) + SCHED_TID_OFFSET;
            std::string sched_name = fmt::format("{} [sched]", base);
            std::string args = fmt::format(R"({{"name":"{}"}})", sched_name);
            writer_.write_metadata("thread_name", tgid, sched_tid, args);
            writer_.write_metadata("thread_sort_index", tgid, sched_tid,
                                   R"({"sort_index":0})");
        }
        if (opts_.include_gil) {
            int64_t gil_tid = static_cast<int64_t>(tid) + GIL_TID_OFFSET;
            std::string gil_name = fmt::format("{} [GIL]", base);
            std::string args = fmt::format(R"({{"name":"{}"}})", gil_name);
            writer_.write_metadata("thread_name", tgid, gil_tid, args);
            writer_.write_metadata("thread_sort_index", tgid, gil_tid,
                                   R"({"sort_index":1})");
        }
    }
}

void MergeEngine::emit_perf_event(const PerfEvent &event) {
    double ts_us = aligner_.align_perf(event.timestamp_ns);

    switch (event.type) {
    case PerfEventType::SchedSwitch: {
        if (!opts_.include_sched) return;

        int32_t prev_tid = event.data.sched_switch.prev_tid;
        int32_t next_tid = event.data.sched_switch.next_tid;
        // Use TGID map — kernel sched_switch "pid" is actually TID
        int32_t prev_tgid = tgid_for(prev_tid);
        int32_t next_tgid = tgid_for(next_tid);
        int64_t sched_prev_tid = static_cast<int64_t>(prev_tid) + SCHED_TID_OFFSET;
        int64_t sched_next_tid = static_cast<int64_t>(next_tid) + SCHED_TID_OFFSET;

        // prev_tid is being switched OUT
        if (passes_filter(prev_tgid)) {
            auto it = sched_state_.find(prev_tid);
            if (it != sched_state_.end() && it->second.on_cpu) {
                // Close on-CPU span: from last_event_ns to now
                double start_us = aligner_.align_perf(it->second.last_event_ns);
                double dur_us = ts_us - start_us;
                if (dur_us > 0) {
                    std::string args = fmt::format(
                        R"({{"cpu":{}}})", it->second.last_cpu);
                    writer_.write_complete(
                        "on-cpu", "sched", start_us, dur_us,
                        prev_tgid, sched_prev_tid, args);
                    perf_written_++;
                }
            }
            // Update state: now off-cpu
            sched_state_[prev_tid] = {
                event.timestamp_ns,
                false,
                event.data.sched_switch.prev_state,
                event.cpu
            };
        }

        // next_tid is being switched IN
        if (passes_filter(next_tgid)) {
            auto it = sched_state_.find(next_tid);
            if (it != sched_state_.end() && !it->second.on_cpu) {
                // Close off-CPU span: from last_event_ns to now
                double start_us = aligner_.align_perf(it->second.last_event_ns);
                double dur_us = ts_us - start_us;
                if (dur_us > 0) {
                    std::string args = fmt::format(
                        R"({{"reason":"{}"}})",
                        prev_state_name(it->second.off_cpu_reason));
                    writer_.write_complete(
                        prev_state_name(it->second.off_cpu_reason), "sched.off-cpu",
                        start_us, dur_us,
                        next_tgid, sched_next_tid, args);
                    perf_written_++;
                }
            }
            // Update state: now on-cpu
            sched_state_[next_tid] = {
                event.timestamp_ns,
                true,
                0,
                event.cpu
            };
        }
        break;
    }

    case PerfEventType::SchedWakeup: {
        if (!opts_.include_sched) return;
        int32_t target_tgid = tgid_for(event.data.wakeup.target_tid);
        if (!passes_filter(target_tgid)) return;

        int64_t sched_tid = static_cast<int64_t>(event.data.wakeup.target_tid) + SCHED_TID_OFFSET;
        std::string args = fmt::format(
            R"({{"target_tid":{}}})", event.data.wakeup.target_tid);

        writer_.write_instant("sched_wakeup", "sched", ts_us,
                              target_tgid, sched_tid, "t", args);
        perf_written_++;
        break;
    }

    case PerfEventType::SchedFork: {
        if (!opts_.include_sched) return;
        int32_t tgid = tgid_for(event.tid);
        if (!passes_filter(tgid)) return;

        int64_t sched_tid = static_cast<int64_t>(event.tid) + SCHED_TID_OFFSET;
        std::string args = fmt::format(
            R"({{"child_pid":{},"child_tid":{}}})",
            event.data.fork.child_pid, event.data.fork.child_tid);

        writer_.write_instant("process_fork", "sched", ts_us,
                              tgid, sched_tid, "t", args);
        perf_written_++;
        break;
    }

    case PerfEventType::TakeGil: {
        if (!opts_.include_gil) return;
        if (!passes_filter(event.pid)) return;

        int64_t gil_tid = static_cast<int64_t>(event.tid) + GIL_TID_OFFSET;
        gil_start_[event.tid] = event.timestamp_ns;
        writer_.write_begin("GIL acquire", "gil", ts_us,
                            event.pid, gil_tid);
        perf_written_++;
        break;
    }

    case PerfEventType::TakeGilReturn: {
        if (!opts_.include_gil) return;
        if (!passes_filter(event.pid)) return;

        int64_t gil_tid = static_cast<int64_t>(event.tid) + GIL_TID_OFFSET;
        auto it = gil_start_.find(event.tid);
        if (it != gil_start_.end()) {
            writer_.write_end("GIL acquire", "gil", ts_us,
                              event.pid, gil_tid);
            perf_written_++;
            gil_start_.erase(it);
        }
        // Start a "GIL held" span — closed by drop_gil
        gil_held_[event.tid] = event.timestamp_ns;
        writer_.write_begin("GIL held", "gil", ts_us,
                            event.pid, gil_tid);
        perf_written_++;
        break;
    }

    case PerfEventType::DropGil: {
        if (!opts_.include_gil) return;
        if (!passes_filter(event.pid)) return;

        int64_t gil_tid = static_cast<int64_t>(event.tid) + GIL_TID_OFFSET;
        auto it = gil_held_.find(event.tid);
        if (it != gil_held_.end()) {
            writer_.write_end("GIL held", "gil", ts_us,
                              event.pid, gil_tid);
            perf_written_++;
            gil_held_.erase(it);
        }
        break;
    }

    // --- NVIDIA CUDA events ---
    case PerfEventType::NvidiaLaunch: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cuLaunchKernel", "gpu", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaLaunchReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cuLaunchKernel", "gpu", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaStreamSync: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cuStreamSynchronize", "gpu.sync", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaStreamSyncReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cuStreamSynchronize", "gpu.sync", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaDeviceSync: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cudaDeviceSynchronize", "gpu.sync", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaDeviceSyncReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cudaDeviceSynchronize", "gpu.sync", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaEventSync: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cuEventSynchronize", "gpu.sync", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaEventSyncReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cuEventSynchronize", "gpu.sync", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    // --- Memory transfers ---
    case PerfEventType::NvidiaMemcpyHtoD: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cuMemcpyHtoD", "gpu.memcpy", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaMemcpyHtoDReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cuMemcpyHtoD", "gpu.memcpy", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaMemcpyDtoH: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cuMemcpyDtoH", "gpu.memcpy", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaMemcpyDtoHReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cuMemcpyDtoH", "gpu.memcpy", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaMemcpyDtoD: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cuMemcpyDtoD", "gpu.memcpy", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaMemcpyDtoDReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cuMemcpyDtoD", "gpu.memcpy", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaMemcpyAsync: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cuMemcpyAsync", "gpu.memcpy", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaMemcpyAsyncReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cuMemcpyAsync", "gpu.memcpy", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaMemcpyPeer: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cuMemcpyPeerAsync", "gpu.memcpy", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaMemcpyPeerReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cuMemcpyPeerAsync", "gpu.memcpy", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    // --- Memory allocation ---
    case PerfEventType::NvidiaMalloc: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cuMemAlloc", "gpu.alloc", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaMallocReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cuMemAlloc", "gpu.alloc", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaFree: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("cuMemFree", "gpu.alloc", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NvidiaFreeReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("cuMemFree", "gpu.alloc", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    // --- NCCL collectives ---
    case PerfEventType::NcclAllReduce: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("ncclAllReduce", "nccl", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NcclAllReduceReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("ncclAllReduce", "nccl", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NcclBroadcast: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("ncclBroadcast", "nccl", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NcclBroadcastReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("ncclBroadcast", "nccl", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NcclReduceScatter: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_begin("ncclReduceScatter", "nccl", ts_us,
                            event.pid, event.tid);
        perf_written_++;
        break;
    }
    case PerfEventType::NcclReduceScatterReturn: {
        if (!opts_.include_gpu || !passes_filter(event.pid)) return;
        writer_.write_end("ncclReduceScatter", "nccl", ts_us,
                          event.pid, event.tid);
        perf_written_++;
        break;
    }

    default:
        break;
    }
}

void MergeEngine::flush_sched_state() {
    if (!opts_.include_sched) return;

    // Find the last perf event timestamp to close open spans
    uint64_t last_ts_ns = 0;
    if (!perf_events_.empty()) {
        last_ts_ns = perf_events_.back().timestamp_ns;
    }
    if (last_ts_ns == 0) return;

    for (const auto &[tid, state] : sched_state_) {
        int32_t tgid = tgid_for(tid);
        if (!passes_filter(tgid)) continue;

        int64_t sched_tid = static_cast<int64_t>(tid) + SCHED_TID_OFFSET;
        double start_us = aligner_.align_perf(state.last_event_ns);
        double end_us = aligner_.align_perf(last_ts_ns);
        double dur_us = end_us - start_us;
        if (dur_us <= 0) continue;

        if (state.on_cpu) {
            std::string args = fmt::format(R"({{"cpu":{}}})", state.last_cpu);
            writer_.write_complete("on-cpu", "sched", start_us, dur_us,
                                   tgid, sched_tid, args);
            perf_written_++;
        } else {
            std::string args = fmt::format(
                R"({{"reason":"{}"}})",
                prev_state_name(state.off_cpu_reason));
            writer_.write_complete(
                prev_state_name(state.off_cpu_reason), "sched.off-cpu",
                start_us, dur_us, tgid, sched_tid, args);
            perf_written_++;
        }
    }
}

void MergeEngine::merge_viz_events(const std::vector<VizEvent> &viz_events) {
    build_tid_map(viz_events);
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

                std::string_view event_name = ve.name;
                std::string cleaned_name;
                if (event_name.substr(0, 11) == "THREAD_MAP:") {
                    cleaned_name = clean_thread_map_name(event_name);
                    event_name = cleaned_name;
                }

                writer_.write_viz_event(ve.ph, event_name, cat,
                                        ts, ve.dur_us, ve.pid, ve.tid, args);
                viz_written_++;
            }
            vi++;
        }
    }

    // Close any remaining open scheduler spans
    flush_sched_state();

    if (opts_.verbose) {
        fmt::print(stderr, "Wrote {} perf events and {} viz events\n",
                   perf_written_, viz_written_);
    }
}

void MergeEngine::write_perf_only() {
    build_tid_map({});
    write_metadata();
    for (const auto &event : perf_events_) {
        emit_perf_event(event);
    }
    flush_sched_state();
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

            std::string_view event_name = ve.name;
            std::string cleaned_name;
            if (event_name.substr(0, 11) == "THREAD_MAP:") {
                cleaned_name = clean_thread_map_name(event_name);
                event_name = cleaned_name;
            }

            writer_.write_viz_event(ve.ph, event_name, cat,
                                    ts, ve.dur_us, ve.pid, ve.tid, args);
            viz_written_++;
        }
    }
    if (opts_.verbose) {
        fmt::print(stderr, "Wrote {} viz events\n", viz_written_);
    }
}
