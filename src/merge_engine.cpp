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

void MergeEngine::build_tid_map(const std::vector<PerfEvent> &fork_events,
                                const std::vector<VizEvent> &viz_events) {
    // From perf fork events: child_tid belongs to child_pid (which IS the tgid)
    for (const auto &event : fork_events) {
        int32_t child_pid = event.data.fork.child_pid;
        int32_t child_tid = event.data.fork.child_tid;
        if (child_pid > 0 && child_tid > 0) {
            tid_to_tgid_[child_tid] = child_pid;
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

void MergeEngine::set_perf_source(
    std::unique_ptr<PerfEventIterator> iter,
    const std::unordered_map<int32_t, std::string> &comm_map,
    const std::vector<PerfEvent> &fork_events,
    uint64_t last_ts_ns) {

    perf_iter_ = std::move(iter);
    comm_map_ = comm_map;
    last_perf_ts_ns_ = last_ts_ns;

    // Build tid_to_tgid_ from fork events and header pid/tid pairs
    for (const auto &event : fork_events) {
        int32_t child_pid = event.data.fork.child_pid;
        int32_t child_tid = event.data.fork.child_tid;
        if (child_pid > 0 && child_tid > 0) {
            tid_to_tgid_[child_tid] = child_pid;
        }
        if (event.pid > 0 && event.tid > 0) {
            tid_to_tgid_[event.tid] = event.pid;
        }
    }
}

void MergeEngine::add_perf_events(
    std::vector<PerfEvent> events,
    const std::unordered_map<int32_t, std::string> &comm_map) {

    comm_map_ = comm_map;

    // Sort by timestamp, with sched_switch last at equal timestamps.
    std::sort(events.begin(), events.end(),
              [](const PerfEvent &a, const PerfEvent &b) {
                  if (a.timestamp_ns != b.timestamp_ns)
                      return a.timestamp_ns < b.timestamp_ns;
                  auto pri = [](PerfEventType t) -> int {
                      return t == PerfEventType::SchedSwitch ? 1 : 0;
                  };
                  return pri(a.type) < pri(b.type);
              });

    // Collect fork events and track last timestamp
    std::vector<PerfEvent> fork_events;
    for (const auto &e : events) {
        if (e.type == PerfEventType::SchedFork)
            fork_events.push_back(e);
        if (e.pid > 0 && e.tid > 0)
            tid_to_tgid_[e.tid] = e.pid;
    }
    last_perf_ts_ns_ = events.empty() ? 0 : events.back().timestamp_ns;

    if (opts_.verbose) {
        std::unordered_map<uint8_t, size_t> type_counts;
        for (const auto &e : events)
            type_counts[static_cast<uint8_t>(e.type)]++;
        auto cname = [](PerfEventType t) -> const char * {
            switch (t) {
            case PerfEventType::SchedSwitch:      return "SchedSwitch";
            case PerfEventType::SchedWakeup:      return "SchedWakeup";
            case PerfEventType::SchedFork:        return "SchedFork";
            case PerfEventType::SchedStatRuntime: return "SchedStatRuntime";
            case PerfEventType::ContextSwitch:    return "ContextSwitch";
            case PerfEventType::TakeGil:          return "TakeGil";
            case PerfEventType::TakeGilReturn:    return "TakeGilReturn";
            case PerfEventType::DropGil:          return "DropGil";
            case PerfEventType::Other:            return "Other";
            default:                              return "GPU/NCCL";
            }
        };
        fmt::print(stderr, "Loaded {} perf events\n", events.size());
        for (const auto &[t, n] : type_counts) {
            fmt::print(stderr, "  {:20s}: {}\n",
                       cname(static_cast<PerfEventType>(t)), n);
        }
    }

    perf_iter_ = std::make_unique<VectorPerfIterator>(std::move(events));
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
    // Also derive process names from tid_to_tgid_ map
    for (const auto &[tid, tgid] : tid_to_tgid_) {
        if (tgid > 0 && pid_names.find(tgid) == pid_names.end()) {
            auto it = comm_map_.find(tgid);
            if (it != comm_map_.end()) {
                pid_names[tgid] = it->second;
            }
        }
    }
    for (const auto &[pid, name] : pid_names) {
        std::string args = fmt::format(R"({{"name":"{}"}})", name);
        writer_.write_metadata("process_name", pid, 0, args);
    }

    // Emit metadata for synthetic GIL, sched, and GPU tracks.
    // Use per-thread sort indices so all tracks for the same thread are
    // adjacent: sched(+0), GIL(+1), call stacks(+2), GPU(+3).
    // Sort tids so the output order is deterministic.
    std::vector<int32_t> sorted_tids(seen_tids.begin(), seen_tids.end());
    std::sort(sorted_tids.begin(), sorted_tids.end());

    for (size_t i = 0; i < sorted_tids.size(); i++) {
        int32_t tid = sorted_tids[i];
        auto comm_it = comm_map_.find(tid);
        std::string base = comm_it != comm_map_.end() ? comm_it->second : std::to_string(tid);
        int32_t tgid = tgid_for(tid);
        int sort_base = static_cast<int>(i) * 4;

        // Sort index for the real thread (call stacks)
        writer_.write_metadata("thread_sort_index", tgid, tid,
                               fmt::format(R"({{"sort_index":{}}})", sort_base + 2));

        if (opts_.include_sched) {
            int64_t sched_tid = static_cast<int64_t>(tid) + SCHED_TID_OFFSET;
            std::string sched_name = fmt::format("{} [sched]", base);
            std::string args = fmt::format(R"({{"name":"{}"}})", sched_name);
            writer_.write_metadata("thread_name", tgid, sched_tid, args);
            writer_.write_metadata("thread_sort_index", tgid, sched_tid,
                                   fmt::format(R"({{"sort_index":{}}})", sort_base + 0));
        }
        if (opts_.include_gil) {
            int64_t gil_tid = static_cast<int64_t>(tid) + GIL_TID_OFFSET;
            std::string gil_name = fmt::format("{} [GIL]", base);
            std::string args = fmt::format(R"({{"name":"{}"}})", gil_name);
            writer_.write_metadata("thread_name", tgid, gil_tid, args);
            writer_.write_metadata("thread_sort_index", tgid, gil_tid,
                                   fmt::format(R"({{"sort_index":{}}})", sort_base + 1));
        }
        if (opts_.include_gpu) {
            int64_t gpu_tid = static_cast<int64_t>(tid) + GPU_TID_OFFSET;
            std::string gpu_name = fmt::format("{} [GPU]", base);
            std::string args = fmt::format(R"({{"name":"{}"}})", gpu_name);
            writer_.write_metadata("thread_name", tgid, gpu_tid, args);
            writer_.write_metadata("thread_sort_index", tgid, gpu_tid,
                                   fmt::format(R"({{"sort_index":{}}})", sort_base + 3));
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

        // Sanity check: sample header tid should match raw prev_tid
        if (opts_.verbose) {
            if (prev_tid == 0 && next_tid == 0) {
                sched_mismatch_count_++;
                if (sched_mismatch_count_ <= 5) {
                    fmt::print(stderr,
                        "WARNING: sched_switch has zero prev_tid AND next_tid "
                        "(raw data not parsed?), header tid={}\n",
                        event.tid);
                }
            } else if (prev_tid != event.tid) {
                sched_mismatch_count_++;
                if (sched_mismatch_count_ <= 5) {
                    fmt::print(stderr,
                        "WARNING: sched_switch prev_tid mismatch: "
                        "raw={} header={} (prev_comm={}, next_tid={})\n",
                        prev_tid, event.tid,
                        event.data.sched_switch.prev_comm, next_tid);
                }
            }
        }

        // Use TGID map — kernel sched_switch "pid" is actually TID
        int32_t prev_tgid = tgid_for(prev_tid);
        int32_t next_tgid = tgid_for(next_tid);

        // Diagnostic counters
        if (opts_.verbose) {
            sched_switch_total_++;
            if (!passes_filter(prev_tgid))
                sched_switch_filtered_++;
            else {
                auto it = sched_state_.find(prev_tid);
                if (it == sched_state_.end())
                    sched_switch_no_state_++;
                else if (!it->second.on_cpu)
                    sched_switch_already_off_++;
                else
                    sched_switch_off_transition_++;
            }
        }
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
        // Only process if next_tid is a known thread of our process.
        // With per-process recording, next_tid is often an external
        // thread — skip those to avoid phantom sched entries.
        if (passes_filter(next_tgid) && tid_to_tgid_.count(next_tid)) {
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
        int32_t target_tid = event.data.wakeup.target_tid;
        if (target_tid == 0) break;  // unparsed wakeup
        int32_t target_tgid = tgid_for(target_tid);
        if (!passes_filter(target_tgid)) return;

        int64_t sched_tid = static_cast<int64_t>(target_tid) + SCHED_TID_OFFSET;

        // Wakeup means the target thread is transitioning from sleeping to
        // runnable.  Use this to close any open off-cpu span — it's the
        // most precise switch-in indicator we have with per-process recording.
        auto it = sched_state_.find(target_tid);
        if (it != sched_state_.end() && !it->second.on_cpu) {
            double start_us = aligner_.align_perf(it->second.last_event_ns);
            double dur_us = ts_us - start_us;
            if (opts_.verbose) {
                offcpu_total_us_ += dur_us;
                offcpu_count_++;
            }
            if (dur_us > 0) {
                std::string args = fmt::format(
                    R"({{"reason":"{}"}})",
                    prev_state_name(it->second.off_cpu_reason));
                writer_.write_complete(
                    prev_state_name(it->second.off_cpu_reason), "sched.off-cpu",
                    start_us, dur_us,
                    target_tgid, sched_tid, args);
                perf_written_++;
            }
            sched_state_[target_tid] = {event.timestamp_ns, true, 0, event.cpu};
        }

        // Also emit the wakeup instant marker
        std::string args = fmt::format(
            R"({{"target_tid":{}}})", target_tid);
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

        // Skip if already acquiring (duplicate probe)
        if (gil_start_.count(event.tid)) break;

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
        } else {
            // Duplicate probe firing — acquire already closed
            break;
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
    // All GPU/NCCL begin/end events use GPU_TID_OFFSET for a separate
    // track and gpu_open_ for deduplication (duplicate probes like
    // nvidia:launch + nvidia:launch_1).
    case PerfEventType::NvidiaLaunch:
    case PerfEventType::NvidiaStreamSync:
    case PerfEventType::NvidiaDeviceSync:
    case PerfEventType::NvidiaEventSync:
    case PerfEventType::NvidiaMemcpyHtoD:
    case PerfEventType::NvidiaMemcpyDtoH:
    case PerfEventType::NvidiaMemcpyDtoD:
    case PerfEventType::NvidiaMemcpyAsync:
    case PerfEventType::NvidiaMemcpyPeer:
    case PerfEventType::NvidiaMalloc:
    case PerfEventType::NvidiaFree:
    case PerfEventType::NcclAllReduce:
    case PerfEventType::NcclBroadcast:
    case PerfEventType::NcclReduceScatter: {
        if (!opts_.include_gpu) return;
        int32_t tgid = tgid_for(event.tid);
        if (!passes_filter(tgid)) return;
        const char *name, *cat;
        switch (event.type) {
        case PerfEventType::NvidiaLaunch:       name = "cuLaunchKernel";       cat = "gpu"; break;
        case PerfEventType::NvidiaStreamSync:   name = "cuStreamSynchronize";  cat = "gpu.sync"; break;
        case PerfEventType::NvidiaDeviceSync:   name = "cudaDeviceSynchronize";cat = "gpu.sync"; break;
        case PerfEventType::NvidiaEventSync:    name = "cuEventSynchronize";   cat = "gpu.sync"; break;
        case PerfEventType::NvidiaMemcpyHtoD:   name = "cuMemcpyHtoD";        cat = "gpu.memcpy"; break;
        case PerfEventType::NvidiaMemcpyDtoH:   name = "cuMemcpyDtoH";        cat = "gpu.memcpy"; break;
        case PerfEventType::NvidiaMemcpyDtoD:   name = "cuMemcpyDtoD";        cat = "gpu.memcpy"; break;
        case PerfEventType::NvidiaMemcpyAsync:  name = "cuMemcpyAsync";        cat = "gpu.memcpy"; break;
        case PerfEventType::NvidiaMemcpyPeer:   name = "cuMemcpyPeerAsync";    cat = "gpu.memcpy"; break;
        case PerfEventType::NvidiaMalloc:       name = "cuMemAlloc";           cat = "gpu.alloc"; break;
        case PerfEventType::NvidiaFree:         name = "cuMemFree";            cat = "gpu.alloc"; break;
        case PerfEventType::NcclAllReduce:      name = "ncclAllReduce";        cat = "nccl"; break;
        case PerfEventType::NcclBroadcast:      name = "ncclBroadcast";        cat = "nccl"; break;
        case PerfEventType::NcclReduceScatter:  name = "ncclReduceScatter";    cat = "nccl"; break;
        default: return;
        }
        // Dedup: skip if this span is already open for this tid
        if (gpu_open_[event.tid].count(name)) break;
        gpu_open_[event.tid].insert(name);
        int64_t gpu_tid = static_cast<int64_t>(event.tid) + GPU_TID_OFFSET;
        writer_.write_begin(name, cat, ts_us, tgid, gpu_tid);
        perf_written_++;
        break;
    }

    case PerfEventType::NvidiaLaunchReturn:
    case PerfEventType::NvidiaStreamSyncReturn:
    case PerfEventType::NvidiaDeviceSyncReturn:
    case PerfEventType::NvidiaEventSyncReturn:
    case PerfEventType::NvidiaMemcpyHtoDReturn:
    case PerfEventType::NvidiaMemcpyDtoHReturn:
    case PerfEventType::NvidiaMemcpyDtoDReturn:
    case PerfEventType::NvidiaMemcpyAsyncReturn:
    case PerfEventType::NvidiaMemcpyPeerReturn:
    case PerfEventType::NvidiaMallocReturn:
    case PerfEventType::NvidiaFreeReturn:
    case PerfEventType::NcclAllReduceReturn:
    case PerfEventType::NcclBroadcastReturn:
    case PerfEventType::NcclReduceScatterReturn: {
        if (!opts_.include_gpu) return;
        int32_t tgid = tgid_for(event.tid);
        if (!passes_filter(tgid)) return;
        const char *name, *cat;
        switch (event.type) {
        case PerfEventType::NvidiaLaunchReturn:       name = "cuLaunchKernel";       cat = "gpu"; break;
        case PerfEventType::NvidiaStreamSyncReturn:   name = "cuStreamSynchronize";  cat = "gpu.sync"; break;
        case PerfEventType::NvidiaDeviceSyncReturn:   name = "cudaDeviceSynchronize";cat = "gpu.sync"; break;
        case PerfEventType::NvidiaEventSyncReturn:    name = "cuEventSynchronize";   cat = "gpu.sync"; break;
        case PerfEventType::NvidiaMemcpyHtoDReturn:   name = "cuMemcpyHtoD";        cat = "gpu.memcpy"; break;
        case PerfEventType::NvidiaMemcpyDtoHReturn:   name = "cuMemcpyDtoH";        cat = "gpu.memcpy"; break;
        case PerfEventType::NvidiaMemcpyDtoDReturn:   name = "cuMemcpyDtoD";        cat = "gpu.memcpy"; break;
        case PerfEventType::NvidiaMemcpyAsyncReturn:  name = "cuMemcpyAsync";        cat = "gpu.memcpy"; break;
        case PerfEventType::NvidiaMemcpyPeerReturn:   name = "cuMemcpyPeerAsync";    cat = "gpu.memcpy"; break;
        case PerfEventType::NvidiaMallocReturn:       name = "cuMemAlloc";           cat = "gpu.alloc"; break;
        case PerfEventType::NvidiaFreeReturn:         name = "cuMemFree";            cat = "gpu.alloc"; break;
        case PerfEventType::NcclAllReduceReturn:      name = "ncclAllReduce";        cat = "nccl"; break;
        case PerfEventType::NcclBroadcastReturn:      name = "ncclBroadcast";        cat = "nccl"; break;
        case PerfEventType::NcclReduceScatterReturn:  name = "ncclReduceScatter";    cat = "nccl"; break;
        default: return;
        }
        // Dedup: only close if we have an open span
        auto oit = gpu_open_.find(event.tid);
        if (oit == gpu_open_.end() || !oit->second.count(name)) break;
        oit->second.erase(name);
        int64_t gpu_tid = static_cast<int64_t>(event.tid) + GPU_TID_OFFSET;
        writer_.write_end(name, cat, ts_us, tgid, gpu_tid);
        perf_written_++;
        break;
    }

    case PerfEventType::ContextSwitch: {
        // context-switches (PERF_COUNT_SW_CONTEXT_SWITCHES) fires on
        // EVERY context switch — both switch-out and switch-in — in
        // the task's perf event context.  With -p PID, this means it
        // fires twice per scheduling cycle for our threads: once at
        // switch-out (same time as sched_switch) and once at switch-in.
        //
        // To distinguish: if the thread was JUST marked off-cpu at the
        // same timestamp by sched_switch, this is the switch-out firing
        // and we must ignore it.  A real switch-in will have a later
        // timestamp than the last sched_switch for this thread.
        if (!opts_.include_sched) return;
        int32_t tid = event.tid;
        int32_t tgid = tgid_for(tid);
        if (!passes_filter(tgid)) return;

        auto it = sched_state_.find(tid);
        if (it == sched_state_.end()) {
            // First event for this thread — set initial on-cpu state
            sched_state_[tid] = {event.timestamp_ns, true, 0, event.cpu};
        } else if (!it->second.on_cpu) {
            // Thread is off-cpu.  context-switches fires for BOTH
            // switch-out and switch-in.  The switch-out event arrives
            // ~1-2μs after sched_switch.  Real switch-ins have a much
            // larger gap (the actual sleep duration).  Filter out the
            // switch-out echo by requiring a minimum gap of 10μs.
            uint64_t gap_ns = event.timestamp_ns - it->second.last_event_ns;
            if (gap_ns > 10000) {  // > 10μs
                int64_t sched_tid = static_cast<int64_t>(tid) + SCHED_TID_OFFSET;
                double start_us = aligner_.align_perf(it->second.last_event_ns);
                double dur_us = ts_us - start_us;
                if (opts_.verbose) {
                    offcpu_total_us_ += dur_us;
                    offcpu_count_++;
                }
                if (dur_us > 0) {
                    std::string args = fmt::format(
                        R"({{"reason":"{}"}})",
                        prev_state_name(it->second.off_cpu_reason));
                    writer_.write_complete(
                        prev_state_name(it->second.off_cpu_reason), "sched.off-cpu",
                        start_us, dur_us,
                        tgid, sched_tid, args);
                    perf_written_++;
                }
                sched_state_[tid] = {event.timestamp_ns, true, 0, event.cpu};
            }
            // else: too close to switch-out → this is the switch-out
            // context-switch echo, ignore it.
            else if (opts_.verbose) {
                cs_same_ts_count_++;
            }
        }
        break;
    }

    case PerfEventType::SchedStatRuntime: {
        // sched_stat_runtime is a periodic ~4ms heartbeat confirming
        // the thread is on-CPU.  It fires from update_curr(), which
        // also runs just before sched_switch — so a sched_stat_runtime
        // at near-identical timestamp as a sched_switch is the pre-
        // switch-out tick, NOT evidence of a switch-in.
        //
        // Use a 10μs minimum gap: if the thread went off-cpu less than
        // 10μs ago, this is the pre-switch tick and should be ignored.
        // Real switch-ins have gaps of at least tens of microseconds.
        if (!opts_.include_sched) return;
        int32_t tid = event.tid;
        int32_t tgid = tgid_for(tid);
        if (!passes_filter(tgid)) return;

        auto it = sched_state_.find(tid);
        if (it == sched_state_.end()) {
            // First event for this thread — set initial on-cpu state
            sched_state_[tid] = {event.timestamp_ns, true, 0, event.cpu};
        } else if (!it->second.on_cpu) {
            uint64_t gap_ns = event.timestamp_ns - it->second.last_event_ns;
            if (gap_ns > 10000) {  // > 10μs
                int64_t sched_tid = static_cast<int64_t>(tid) + SCHED_TID_OFFSET;
                double start_us = aligner_.align_perf(it->second.last_event_ns);
                double dur_us = ts_us - start_us;
                if (opts_.verbose) {
                    offcpu_total_us_ += dur_us;
                    offcpu_count_++;
                }
                if (dur_us > 0) {
                    std::string args = fmt::format(
                        R"({{"reason":"{}"}})",
                        prev_state_name(it->second.off_cpu_reason));
                    writer_.write_complete(
                        prev_state_name(it->second.off_cpu_reason), "sched.off-cpu",
                        start_us, dur_us,
                        tgid, sched_tid, args);
                    perf_written_++;
                }
                sched_state_[tid] = {event.timestamp_ns, true, 0, event.cpu};
            }
        }
        // If already on-cpu: no-op
        break;
    }

    default:
        break;
    }
}

void MergeEngine::flush_sched_state() {
    if (!opts_.include_sched) return;

    uint64_t last_ts_ns = last_perf_ts_ns_;
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

void MergeEngine::maybe_report_progress() {
    static constexpr uint64_t INTERVAL = 1'000'000;
    progress_counter_++;
    if (progress_counter_ % INTERVAL != 0) return;
    uint64_t total = perf_written_ + viz_written_;
    if (total_viz_events_ > 0) {
        double pct = 100.0 * viz_written_ / total_viz_events_;
        fmt::print(stderr, "\r  Progress: {} perf + {} viz events written ({:.1f}% of viz)   ",
                   perf_written_, viz_written_, pct);
    } else {
        fmt::print(stderr, "\r  Progress: {} perf + {} viz events written   ",
                   perf_written_, viz_written_);
    }
}

void MergeEngine::merge_viz_events(const std::vector<VizEvent> &viz_events) {
    // Build tid map from fork events collected during read + viz events
    std::vector<PerfEvent> empty_forks; // fork events already loaded in set_perf_source
    build_tid_map(empty_forks, viz_events);
    write_metadata();
    total_viz_events_ = viz_events.size();

    // Two-pointer merge of time-sorted perf events and viz events
    size_t vi = 0; // viz index

    while ((perf_iter_ && perf_iter_->has_next()) || vi < viz_events.size()) {
        bool emit_perf = false;

        if (!perf_iter_ || !perf_iter_->has_next()) {
            emit_perf = false; // Only viz left
        } else if (vi >= viz_events.size()) {
            emit_perf = true; // Only perf left
        } else {
            // Compare timestamps (convert both to microseconds)
            double perf_us = aligner_.align_perf(perf_iter_->peek().timestamp_ns);
            double viz_us = aligner_.align_viz(viz_events[vi].ts_us);
            emit_perf = (perf_us <= viz_us);
        }

        if (emit_perf) {
            emit_perf_event(perf_iter_->peek());
            perf_iter_->advance();
            if (opts_.verbose) maybe_report_progress();
        } else {
            const VizEvent &ve = viz_events[vi];
            double ts = aligner_.align_viz(ve.ts_us);

            if (passes_filter(static_cast<int32_t>(ve.pid)) &&
                (ve.ph != 'X' || ve.dur_us >= opts_.min_duration_us)) {
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
            if (opts_.verbose) maybe_report_progress();
        }
    }

    // Close any remaining open scheduler spans
    flush_sched_state();

    if (opts_.verbose) {
        fmt::print(stderr, "\n");
        fmt::print(stderr, "Wrote {} perf events and {} viz events\n",
                   perf_written_, viz_written_);
        if (sched_mismatch_count_ > 0) {
            fmt::print(stderr,
                "WARNING: {} sched_switch events had prev_tid mismatch "
                "(raw data offsets may be wrong for this kernel)\n",
                sched_mismatch_count_);
        }
        fmt::print(stderr,
            "Sched switch breakdown: total={} filtered={} no_state={} "
            "already_off={} off_transition={}\n",
            sched_switch_total_, sched_switch_filtered_,
            sched_switch_no_state_, sched_switch_already_off_,
            sched_switch_off_transition_);
        if (offcpu_count_ > 0) {
            fmt::print(stderr,
                "Off-cpu spans: count={} total={:.1f}s avg={:.1f}us "
                "(cs_same_ts_filtered={})\n",
                offcpu_count_, offcpu_total_us_ / 1e6,
                offcpu_total_us_ / offcpu_count_,
                cs_same_ts_count_);
        }
    }
}

void MergeEngine::write_perf_only() {
    std::vector<PerfEvent> empty_forks;
    std::vector<VizEvent> empty_viz;
    build_tid_map(empty_forks, empty_viz);
    write_metadata();
    if (perf_iter_) {
        while (perf_iter_->has_next()) {
            emit_perf_event(perf_iter_->peek());
            perf_iter_->advance();
            if (opts_.verbose) maybe_report_progress();
        }
    }
    flush_sched_state();
    if (opts_.verbose) {
        fmt::print(stderr, "\nWrote {} perf events\n", perf_written_);
    }
}

void MergeEngine::write_viz_only(const std::vector<VizEvent> &viz_events) {
    total_viz_events_ = viz_events.size();
    for (const auto &ve : viz_events) {
        double ts = aligner_.align_viz(ve.ts_us);
        if (passes_filter(static_cast<int32_t>(ve.pid)) &&
            (ve.ph != 'X' || ve.dur_us >= opts_.min_duration_us)) {
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
        if (opts_.verbose) maybe_report_progress();
    }
    if (opts_.verbose) {
        fmt::print(stderr, "\nWrote {} viz events\n", viz_written_);
    }
}
