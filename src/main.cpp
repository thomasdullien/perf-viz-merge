#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
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
#include "chunking_writer.h"
#include "kway_viz_merge.h"
#include "metric_csv_reader.h"
#include "streaming_sort.h"

// close_open_stacks() removed — libftrc handles entry/exit pairing
// internally, so ftrc events are always properly closed.

static void usage(const char *prog) {
    fmt::print(stderr,
        "Usage: {} [options]\n"
        "\n"
        "Options:\n"
        "  --perf <path>          Path to perf.data file\n"
        "  --viz <path>           Path to VizTracer JSON file (may be repeated)\n"
        "  -o, --output <path>    Output file (default: merged.perfetto-trace)\n"
        "  --time-offset <us>     Manual time offset in microseconds\n"
        "  --clock-monotonic      Assume both perf and viz use CLOCK_MONOTONIC\n"
        "                         (no auto-detect, force offset=0; use viz time\n"
        "                         range for chunking).\n"
        "  --filter-pid <pid>     Only include events for this PID\n"
        "  --filter-name <pat>    Only include events from threads/processes matching\n"
        "                         substring (may be repeated; matches are OR'd)\n"
        "  --time-start <sec>     Only include events starting at this many seconds\n"
        "                         from the beginning of the trace\n"
        "  --time-end <sec>       Only include events up to this many seconds\n"
        "                         from the beginning of the trace\n"
        "  --chunk-duration <sec> Split output into multiple files, each covering\n"
        "                         this many seconds of trace time\n"
        "  --cpu-metrics <csv>    CSV file with CPU/memory utilization metrics\n"
        "  --gpu-metrics <csv>    CSV file with GPU utilization metrics\n"
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
    double chunk_duration_s = -1;  // -1 = no chunking
    std::string cpu_metrics_path;
    std::string gpu_metrics_path;
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
        } else if (arg == "--clock-monotonic") {
            opts.force_clock_monotonic = true;
        } else if (arg == "--filter-pid") {
            opts.filter_pid = std::atoi(next());
        } else if (arg == "--filter-name") {
            opts.filter_names.push_back(next());
        } else if (arg == "--time-start") {
            opts.time_start_s = std::atof(next());
        } else if (arg == "--time-end") {
            opts.time_end_s = std::atof(next());
        } else if (arg == "--chunk-duration") {
            chunk_duration_s = std::atof(next());
            if (chunk_duration_s <= 0) {
                fmt::print(stderr, "Error: --chunk-duration must be positive\n");
                return 1;
            }
        } else if (arg == "--cpu-metrics") {
            cpu_metrics_path = next();
        } else if (arg == "--gpu-metrics") {
            gpu_metrics_path = next();
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
    if (opts.force_clock_monotonic) {
        aligner.force_clock_monotonic();
    }

    // Read perf events
    std::unique_ptr<PerfEventIterator> perf_iter;
    std::vector<PerfEvent> perf_events_vec;  // kept for chunked replay
    std::vector<PerfEvent> fork_events;
    std::unordered_map<int32_t, std::string> comm_map;
    uint64_t perf_min_ts = 0, perf_max_ts = 0;
    uint64_t perf_event_count = 0;
    bool has_perf = false;

    static constexpr size_t STREAMING_THRESHOLD = 8ULL * 1024 * 1024 * 1024;  // 8GB
    static constexpr size_t REORDER_BUFFER_SIZE = 1'000'000;  // ~88MB

    // Chunking re-opens perf file per chunk via streaming iterator,
    // so we never need to load all perf events into memory.
    bool need_perf_in_memory = false;

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
                {
                    reader->read_all_events([&](const PerfEvent &event) {
                        perf_events_vec.push_back(event);
                        if (event.type == PerfEventType::SchedFork)
                            fork_events.push_back(event);
                    });
                    comm_map = reader->comm_map();

                    // Sort with SchedSwitch-last tiebreaker
                    std::sort(perf_events_vec.begin(), perf_events_vec.end(),
                              [](const PerfEvent &a, const PerfEvent &b) {
                                  if (a.timestamp_ns != b.timestamp_ns)
                                      return a.timestamp_ns < b.timestamp_ns;
                                  auto pri = [](PerfEventType t) -> int {
                                      return t == PerfEventType::SchedSwitch ? 1 : 0;
                                  };
                                  return pri(a.type) < pri(b.type);
                              });
                }

                perf_event_count = perf_events_vec.size();
                if (!perf_events_vec.empty()) {
                    perf_min_ts = perf_events_vec.front().timestamp_ns;
                    perf_max_ts = perf_events_vec.back().timestamp_ns;
                }

                // Build fork_events from loaded data if not already populated
                if (fork_events.empty()) {
                    for (const auto &e : perf_events_vec) {
                        if (e.type == PerfEventType::SchedFork)
                            fork_events.push_back(e);
                    }
                }

                if (!need_perf_in_memory) {
                    // Non-chunked: move into iterator (single pass)
                    perf_iter = std::make_unique<VectorPerfIterator>(
                        std::move(perf_events_vec));
                }
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

    // --- Pre-scan ftrc files (streaming, O(1) memory per file) ---
    // Collect metadata events, min/max timestamps, and TID→PID mappings
    // without loading all events into memory.

    double viz_first = std::numeric_limits<double>::max();
    double viz_last = std::numeric_limits<double>::lowest();
    std::vector<VizEvent> viz_metadata;
    std::unordered_map<int64_t, int32_t> viz_tid_to_pid;
    bool has_viz = !viz_paths.empty();

    for (const auto &viz_path : viz_paths) {
        if (opts.verbose) {
            fmt::print(stderr, "Pre-scanning {}\n", viz_path);
        }
        try {
            FtrcReader scanner(viz_path);
            scanner.read_all_events([&](const VizEvent &ev) {
                if (ev.ts_us < viz_first) viz_first = ev.ts_us;
                if (ev.ts_us > viz_last) viz_last = ev.ts_us;
                if (ev.ph == 'M') {
                    viz_metadata.push_back(ev);
                }
                viz_tid_to_pid[ev.tid] = static_cast<int32_t>(ev.pid);
            });
            if (opts.verbose) {
                fmt::print(stderr, "  {} events scanned from {}\n",
                           scanner.event_count(), viz_path);
            }
        } catch (const std::exception &e) {
            fmt::print(stderr, "Error scanning {}: {}\n", viz_path, e.what());
            return 1;
        }
    }

    // Auto-detect clock alignment if both sources are present (unless the
    // user has supplied --time-offset or --clock-monotonic, both of which
    // pin the offset).
    if (has_perf && has_viz && !has_time_offset && !opts.force_clock_monotonic) {
        uint64_t perf_first = perf_min_ts;
        uint64_t perf_last = perf_max_ts;

        if (opts.verbose) {
            double pf_us = static_cast<double>(perf_first) / 1000.0;
            double pl_us = static_cast<double>(perf_last) / 1000.0;
            fmt::print(stderr, "Perf  timestamps: {:.3f} - {:.3f} us (range {:.1f}s)\n",
                       pf_us, pl_us, (pl_us - pf_us) / 1e6);
            fmt::print(stderr, "Viz   timestamps: {:.3f} - {:.3f} us (range {:.1f}s)\n",
                       viz_first, viz_last, (viz_last - viz_first) / 1e6);
            fmt::print(stderr, "Start difference: {:.3f}s (perf - viz)\n",
                       (static_cast<double>(perf_first) / 1000.0 - viz_first) / 1e6);
        }

        aligner.detect(static_cast<double>(perf_first),
                        static_cast<double>(perf_last),
                        viz_first, viz_last);

        if (opts.verbose) {
            fmt::print(stderr, "Clock alignment offset: {:.3f} us\n",
                       aligner.offset_us());
        }
    }

    // --- Open ftrc files for streaming k-way merge ---
    // Each file is re-opened for pull-based iteration. The kernel
    // page cache means this second pass is fast (no disk I/O).

    std::vector<std::unique_ptr<FtrcReader>> ftrc_readers;
    KWayVizMerge kway_merge;

    for (const auto &viz_path : viz_paths) {
        auto reader = std::make_unique<FtrcReader>(viz_path);
        kway_merge.add_source(reader.get());
        ftrc_readers.push_back(std::move(reader));
    }

    if (has_viz) {
        kway_merge.initialize();
        if (opts.verbose) {
            fmt::print(stderr, "K-way merge initialized with {} sources\n",
                       ftrc_readers.size());
        }
    }

    // Load metric CSV files (if provided)
    std::vector<MetricSample> metric_samples;
    if (!cpu_metrics_path.empty()) {
        MetricCsvReader reader(cpu_metrics_path);
        auto samples = reader.read_all("CPU ");
        if (opts.verbose) {
            fmt::print(stderr, "Read {} CPU metric samples from {}\n",
                       samples.size(), cpu_metrics_path);
        }
        metric_samples.insert(metric_samples.end(),
                              std::make_move_iterator(samples.begin()),
                              std::make_move_iterator(samples.end()));
    }
    if (!gpu_metrics_path.empty()) {
        MetricCsvReader reader(gpu_metrics_path);
        auto samples = reader.read_all("GPU ");
        if (opts.verbose) {
            fmt::print(stderr, "Read {} GPU metric samples from {}\n",
                       samples.size(), gpu_metrics_path);
        }
        metric_samples.insert(metric_samples.end(),
                              std::make_move_iterator(samples.begin()),
                              std::make_move_iterator(samples.end()));
    }
    if (!metric_samples.empty()) {
        std::sort(metric_samples.begin(), metric_samples.end(),
                  [](const MetricSample &a, const MetricSample &b) {
                      return a.ts_us < b.ts_us;
                  });
    }

    // Helper to write metrics to a PerfettoWriter within a time range
    auto write_metrics = [&](PerfettoWriter &writer, int64_t pid,
                             double start_us = -1, double end_us = -1) {
        size_t count = 0;
        for (const auto &m : metric_samples) {
            double ts = aligner.align_viz(m.ts_us);
            if (start_us >= 0 && ts < start_us) continue;
            if (end_us >= 0 && ts > end_us) continue;
            writer.write_counter(m.name, ts, m.value, pid);
            count++;
        }
        if (opts.verbose && count > 0) {
            fmt::print(stderr, "Wrote {} metric counter events\n", count);
        }
    };

    // Helper lambda to run a single merge pass with given options and output path
    auto run_merge = [&](const std::string &out_path, MergeEngine::Options &chunk_opts,
                         bool reopen_perf = false) {
        auto writer = std::make_unique<PerfettoWriter>(out_path);

        MergeEngine engine(*writer, aligner, chunk_opts);

        if (has_perf) {
            if (reopen_perf && !perf_path.empty()) {
                // Re-open perf file with a fresh streaming iterator per chunk
                auto reader = std::make_unique<PerfDataReader>(perf_path);
                auto reorder = std::make_unique<ReorderBufferIterator>(
                    std::move(reader), REORDER_BUFFER_SIZE);
                engine.set_perf_source(std::move(reorder), comm_map,
                                       fork_events, perf_max_ts);
            } else if (!perf_events_vec.empty()) {
                auto events_copy = perf_events_vec;
                auto iter = std::make_unique<VectorPerfIterator>(std::move(events_copy));
                engine.set_perf_source(std::move(iter), comm_map,
                                       fork_events, perf_max_ts);
            } else if (perf_iter) {
                engine.set_perf_source(std::move(perf_iter), comm_map,
                                       fork_events, perf_max_ts);
            }
        }

        // Set pre-scanned metadata
        if (has_viz) {
            engine.set_viz_metadata(viz_metadata, viz_tid_to_pid,
                                    viz_first, viz_last);
        }

        if (has_perf && has_viz) {
            engine.merge_viz_events(kway_merge);
        } else if (has_perf) {
            engine.write_perf_only();
        } else if (has_viz) {
            engine.write_viz_only(kway_merge);
        }

        // Write metric counter events (CPU/GPU utilization)
        if (!metric_samples.empty()) {
            write_metrics(*writer, 0);
        }

        writer->finalize();
        return writer->events_written();
    };

    // Determine output file stem and extension for chunked filenames
    auto split_path = [](const std::string &path)
        -> std::pair<std::string, std::string> {
        // Find the last dot that is after any directory separator
        auto last_sep = path.find_last_of('/');
        auto last_dot = path.find_last_of('.');
        if (last_dot != std::string::npos &&
            (last_sep == std::string::npos || last_dot > last_sep)) {
            return {path.substr(0, last_dot), path.substr(last_dot)};
        }
        return {path, ""};
    };

    try {
        if (chunk_duration_s > 0) {
            // --- Chunked output (single-pass) ---
            //
            // Uses ChunkingWriter to swap the underlying PerfettoWriter
            // at chunk boundaries during a single pass through the data.
            // This avoids re-reading the perf.data file for each chunk.

            // Determine the global time range across all sources (in aligned us)
            double global_min_us = std::numeric_limits<double>::max();
            double global_max_us = std::numeric_limits<double>::lowest();

            // In force_clock_monotonic mode with viz present, derive chunking
            // range from viz only. Both sources share the clock, viz captures
            // the actual workload duration, and any stray perf events outside
            // this range (e.g. metadata records with bogus timestamps) get
            // clamped to chunk 0 or the last chunk -- a couple of misplaced
            // events instead of the whole perf stream piling into one chunk.
            bool use_viz_only_for_range = opts.force_clock_monotonic && has_viz;
            if (has_perf && perf_min_ts > 0 && !use_viz_only_for_range) {
                double pmin = aligner.align_perf(perf_min_ts);
                double pmax = aligner.align_perf(perf_max_ts);
                if (pmin < global_min_us) global_min_us = pmin;
                if (pmax > global_max_us) global_max_us = pmax;
            }
            if (has_viz) {
                double vmin = aligner.align_viz(viz_first);
                double vmax = aligner.align_viz(viz_last);
                if (vmin < global_min_us) global_min_us = vmin;
                if (vmax > global_max_us) global_max_us = vmax;
            }

            if (global_min_us > global_max_us) {
                fmt::print(stderr, "Error: no events found for chunking\n");
                return 1;
            }

            double total_duration_s = (global_max_us - global_min_us) / 1e6;
            double chunk_duration_us = chunk_duration_s * 1e6;
            int num_chunks = static_cast<int>(
                std::ceil(total_duration_s / chunk_duration_s));
            if (num_chunks < 1) num_chunks = 1;

            auto [stem, ext] = split_path(output_path);

            if (opts.verbose) {
                fmt::print(stderr, "Chunked output: {:.1f}s total, {:.1f}s per chunk, "
                           "{} chunks (single-pass)\n",
                           total_duration_s, chunk_duration_s, num_chunks);
            }

            // Create the chunking writer
            auto chunker = std::make_unique<ChunkingWriter>(
                chunk_duration_us, global_min_us,
                stem, ext, num_chunks, opts.verbose);

            // Inject metric samples so each chunk's open_chunk() emits the
            // subset within its time window. Without this, --cpu-metrics
            // and --gpu-metrics are silently dropped in chunked mode.
            if (!metric_samples.empty()) {
                chunker->set_metric_samples(metric_samples);
            }

            // Context spans are handled automatically by ChunkingWriter's
            // per-thread open stack tracking — no external callback needed.

            // Run a single merge pass — the chunking writer handles file splitting
            MergeEngine engine(*chunker, aligner, opts);

            if (perf_iter) {
                engine.set_perf_source(std::move(perf_iter), comm_map,
                                       fork_events, perf_max_ts);
            }

            if (has_viz) {
                engine.set_viz_metadata(viz_metadata, viz_tid_to_pid,
                                        viz_first, viz_last);
            }

            if (has_perf && has_viz) {
                engine.merge_viz_events(kway_merge);
            } else if (has_perf) {
                engine.write_perf_only();
            } else if (has_viz) {
                engine.write_viz_only(kway_merge);
            }

            chunker->finalize();

            if (opts.verbose) {
                fmt::print(stderr, "Done. {} chunks, {} total events\n",
                           chunker->chunks_written(), chunker->events_written());
            }
        } else {
            // --- Single output (original behavior) ---
            if (opts.verbose) {
                fmt::print(stderr, "Writing Perfetto output to {}\n", output_path);
            }

            uint64_t events = run_merge(output_path, opts);

            if (opts.verbose) {
                fmt::print(stderr, "Done. Total events written: {}\n", events);
            }
        }
    } catch (const std::exception &e) {
        fmt::print(stderr, "Error writing output: {}\n", e.what());
        return 1;
    }

    return 0;
}
