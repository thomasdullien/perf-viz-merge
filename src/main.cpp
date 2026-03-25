#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "clock_aligner.h"
#include "event_types.h"
#include "ftrc_reader.h"
#include "merge_engine.h"
#include "output_writer.h"
#include "perf_data_reader.h"
#include "perfetto_writer.h"
#include "streaming_sort.h"
#include "viz_json_reader.h"

// Close any open B (begin) events left at the end of a trace file by
// appending synthetic E (end) events.  Without this, unclosed call stacks
// from one file bleed into the next file on the same tid.
static void close_open_stacks(std::vector<VizEvent> &events, size_t start_idx) {
    // Build per-tid stack of open B events (index into events vector)
    std::unordered_map<int64_t, std::vector<size_t>> stacks;

    for (size_t i = start_idx; i < events.size(); i++) {
        const auto &e = events[i];
        if (e.ph == 'B') {
            stacks[e.tid].push_back(i);
        } else if (e.ph == 'E') {
            auto &st = stacks[e.tid];
            if (!st.empty()) st.pop_back();
        }
        // X events are self-contained, no stack effect
    }

    // Find the last timestamp in this file's events
    double last_ts = 0;
    for (size_t i = start_idx; i < events.size(); i++) {
        if (events[i].ts_us > last_ts) last_ts = events[i].ts_us;
    }

    // Emit synthetic E events for each unclosed B, in reverse stack order
    for (auto &[tid, st] : stacks) {
        for (auto it = st.rbegin(); it != st.rend(); ++it) {
            VizEvent close;
            close.ts_us = last_ts;
            close.dur_us = 0;
            close.pid = events[*it].pid;
            close.tid = tid;
            close.ph = 'E';
            close.name = events[*it].name;
            close.cat = events[*it].cat;
            events.push_back(std::move(close));
        }
    }
}

static void usage(const char *prog) {
    fmt::print(stderr,
        "Usage: {} [options]\n"
        "\n"
        "Options:\n"
        "  --perf <path>          Path to perf.data file\n"
        "  --viz <path>           Path to VizTracer JSON file (may be repeated)\n"
        "  -o, --output <path>    Output file (default: merged.perfetto-trace)\n"
        "  --time-offset <us>     Manual time offset in microseconds\n"
        "  --filter-pid <pid>     Only include events for this PID\n"
        "  --no-sched             Omit scheduler events\n"
        "  --no-gil               Omit GIL tracking events\n"
        "  --no-gpu               Omit GPU/NCCL events\n"
        "  --min-duration <us>    Skip viz events shorter than this (microseconds)\n"
        "  -v, --verbose          Print progress information\n"
        "  -h, --help             Show this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    std::string perf_path;
    std::vector<std::string> viz_paths;
    std::string output_path;
    double time_offset = 0;
    bool has_time_offset = false;
    MergeEngine::Options opts;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        auto arg = std::string_view(argv[i]);
        auto next = [&]() -> const char * {
            if (i + 1 < argc) return argv[++i];
            fmt::print(stderr, "Error: {} requires a value\n", arg);
            std::exit(1);
            return nullptr;
        };

        if (arg == "--perf") {
            perf_path = next();
        } else if (arg == "--viz") {
            viz_paths.push_back(next());
        } else if (arg == "-o" || arg == "--output") {
            output_path = next();
        } else if (arg == "--time-offset") {
            time_offset = std::atof(next());
            has_time_offset = true;
        } else if (arg == "--filter-pid") {
            opts.filter_pid = std::atoi(next());
        } else if (arg == "--no-sched") {
            opts.include_sched = false;
        } else if (arg == "--no-gil") {
            opts.include_gil = false;
        } else if (arg == "--no-gpu") {
            opts.include_gpu = false;
        } else if (arg == "--min-duration") {
            opts.min_duration_us = std::atof(next());
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            fmt::print(stderr, "Unknown option: {}\n", arg);
            usage(argv[0]);
            return 1;
        }
    }

    if (perf_path.empty() && viz_paths.empty()) {
        fmt::print(stderr, "Error: at least one of --perf or --viz is required\n");
        usage(argv[0]);
        return 1;
    }

    if (output_path.empty()) {
        output_path = "merged.perfetto-trace";
    }

    // Clock aligner
    ClockAligner aligner;
    if (has_time_offset) {
        aligner.set_manual_offset(time_offset);
    }

    // Read perf events
    std::unique_ptr<PerfEventIterator> perf_iter;
    std::vector<PerfEvent> fork_events;
    std::unordered_map<int32_t, std::string> comm_map;
    uint64_t perf_min_ts = 0, perf_max_ts = 0;
    uint64_t perf_event_count = 0;
    bool has_perf = false;

    static constexpr size_t STREAMING_THRESHOLD = 8ULL * 1024 * 1024 * 1024;  // 8GB
    static constexpr size_t REORDER_BUFFER_SIZE = 1'000'000;  // ~88MB

    if (!perf_path.empty()) {
        if (opts.verbose) {
            fmt::print(stderr, "Reading perf data from {}\n", perf_path);
        }

        try {
            auto reader = std::make_unique<PerfDataReader>(perf_path);
            size_t file_size = reader->file_size();
            comm_map = reader->comm_map();

            if (opts.verbose) {
                fmt::print(stderr, "Perf file size: {:.1f} GB ({} event types)\n",
                           file_size / (1024.0 * 1024.0 * 1024.0),
                           reader->event_names().size());
                for (size_t i = 0; i < reader->event_names().size(); i++) {
                    if (!reader->event_names()[i].empty()) {
                        fmt::print(stderr, "  Event {}: {}\n", i, reader->event_names()[i]);
                    }
                }
            }

            if (file_size < STREAMING_THRESHOLD) {
                // Small-file fast path: load all events, sort in-memory
                if (opts.verbose) {
                    fmt::print(stderr, "Using in-memory sort (file < 8GB)\n");
                }
                std::vector<PerfEvent> perf_events;
                reader->read_all_events([&](const PerfEvent &event) {
                    perf_events.push_back(event);
                    if (event.type == PerfEventType::SchedFork)
                        fork_events.push_back(event);
                });
                comm_map = reader->comm_map();
                perf_event_count = perf_events.size();

                // Sort with SchedSwitch-last tiebreaker
                std::sort(perf_events.begin(), perf_events.end(),
                          [](const PerfEvent &a, const PerfEvent &b) {
                              if (a.timestamp_ns != b.timestamp_ns)
                                  return a.timestamp_ns < b.timestamp_ns;
                              auto pri = [](PerfEventType t) -> int {
                                  return t == PerfEventType::SchedSwitch ? 1 : 0;
                              };
                              return pri(a.type) < pri(b.type);
                          });

                if (!perf_events.empty()) {
                    perf_min_ts = perf_events.front().timestamp_ns;
                    perf_max_ts = perf_events.back().timestamp_ns;
                }

                perf_iter = std::make_unique<VectorPerfIterator>(std::move(perf_events));
            } else {
                // Large-file path: streaming reorder buffer
                if (opts.verbose) {
                    fmt::print(stderr, "Using streaming reorder buffer "
                               "(file >= 8GB, buffer = {} events = {:.0f}MB)\n",
                               REORDER_BUFFER_SIZE,
                               REORDER_BUFFER_SIZE * sizeof(PerfEvent) / (1024.0 * 1024.0));
                }
                auto reorder = std::make_unique<ReorderBufferIterator>(
                    std::move(reader), REORDER_BUFFER_SIZE);

                fork_events = reorder->fork_events();
                perf_min_ts = reorder->min_timestamp_ns();
                perf_max_ts = reorder->max_timestamp_ns();
                // Note: total_events() reflects the initial fill count;
                // the full count is only known after iteration completes.
                perf_event_count = reorder->total_events();
                // comm_map already captured above before reader was moved
                perf_iter = std::move(reorder);
            }

            has_perf = true;

            if (opts.verbose) {
                fmt::print(stderr, "Read {} perf events\n", perf_event_count);
            }
        } catch (const std::exception &e) {
            fmt::print(stderr, "Error reading perf data: {}\n", e.what());
            return 1;
        }
    }

    // Read VizTracer events
    std::vector<VizEvent> viz_events;

    // Pre-estimate capacity from file sizes to avoid repeated reallocations.
    // For ftrc files: ~12 bytes/raw event, ~2 raw events per completed event,
    // plus header overhead. Rough estimate: file_size / 24.
    {
        size_t estimated = 0;
        for (const auto &vp : viz_paths) {
            struct stat st;
            if (stat(vp.c_str(), &st) == 0) {
                estimated += static_cast<size_t>(st.st_size) / 24;
            }
        }
        if (estimated > 0) {
            viz_events.reserve(estimated);
        }
    }

    for (const auto &viz_path : viz_paths) {
        if (opts.verbose) {
            fmt::print(stderr, "Reading trace data from {}\n", viz_path);
        }

        size_t before = viz_events.size();
        try {
            // Detect file type by extension
            bool is_ftrc = viz_path.size() >= 5 &&
                           viz_path.substr(viz_path.size() - 5) == ".ftrc";

            if (is_ftrc) {
                FtrcReader reader(viz_path);
                reader.read_all_events([&](const VizEvent &event) {
                    viz_events.push_back(event);
                });
                if (opts.verbose) {
                    fmt::print(stderr, "Read {} events from {} (ftrc)\n",
                               reader.event_count(), viz_path);
                }
            } else {
                VizJsonReader reader(viz_path);
                reader.read_all_events([&](const VizEvent &event) {
                    viz_events.push_back(event);
                });
                if (opts.verbose) {
                    fmt::print(stderr, "Read {} events from {} (json)\n",
                               reader.event_count(), viz_path);
                }
            }
        } catch (const std::exception &e) {
            fmt::print(stderr, "Error reading trace data from {}: {}\n",
                       viz_path, e.what());
            return 1;
        }

        // Close any call stacks left open at the end of this file
        // so they don't bleed into the next file's events.
        if (viz_events.size() > before) {
            size_t before_close = viz_events.size();
            close_open_stacks(viz_events, before);
            if (opts.verbose && viz_events.size() > before_close) {
                fmt::print(stderr, "  Closed {} open call stacks\n",
                           viz_events.size() - before_close);
            }
        }
    }

    // Auto-detect clock alignment if both sources are present
    if (has_perf && !viz_events.empty() && !has_time_offset) {
        uint64_t perf_first = perf_min_ts;
        uint64_t perf_last = perf_max_ts;

        double viz_first = viz_events.front().ts_us;
        double viz_last = viz_events.front().ts_us;
        for (const auto &e : viz_events) {
            if (e.ts_us < viz_first) viz_first = e.ts_us;
            if (e.ts_us > viz_last) viz_last = e.ts_us;
        }

        if (opts.verbose) {
            double pf_us = static_cast<double>(perf_first) / 1000.0;
            double pl_us = static_cast<double>(perf_last) / 1000.0;
            fmt::print(stderr, "Perf  timestamps: {:.3f} - {:.3f} us (range {:.1f}s)\n",
                       pf_us, pl_us, (pl_us - pf_us) / 1e6);
            fmt::print(stderr, "Viz   timestamps: {:.3f} - {:.3f} us (range {:.1f}s)\n",
                       viz_first, viz_last, (viz_last - viz_first) / 1e6);
            fmt::print(stderr, "Start difference: {:.3f}s (perf - viz)\n",
                       (pf_us - viz_first) / 1e6);
        }

        aligner.detect(static_cast<double>(perf_first),
                        static_cast<double>(perf_last),
                        viz_first, viz_last);

        if (opts.verbose) {
            fmt::print(stderr, "Clock alignment offset: {:.3f} us\n",
                       aligner.offset_us());
        }
    }

    // Sort viz events by timestamp
    std::sort(viz_events.begin(), viz_events.end(),
              [](const VizEvent &a, const VizEvent &b) {
                  return a.ts_us < b.ts_us;
              });

    // Create output
    if (opts.verbose) {
        fmt::print(stderr, "Writing Perfetto output to {}\n", output_path);
    }

    try {
        auto writer = std::make_unique<PerfettoWriter>(output_path);

        MergeEngine engine(*writer, aligner, opts);

        if (perf_iter) {
            engine.set_perf_source(std::move(perf_iter), comm_map,
                                   fork_events, perf_max_ts);
        }

        if (has_perf && !viz_paths.empty()) {
            engine.merge_viz_events(viz_events);
        } else if (has_perf) {
            engine.write_perf_only();
        } else {
            engine.write_viz_only(viz_events);
        }

        writer->finalize();

        if (opts.verbose) {
            fmt::print(stderr, "Done. Total events written: {}\n",
                       writer->events_written());
        }
    } catch (const std::exception &e) {
        fmt::print(stderr, "Error writing output: {}\n", e.what());
        return 1;
    }

    return 0;
}
