#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "clock_aligner.h"
#include "event_types.h"
#include "merge_engine.h"
#include "output_writer.h"
#include "perf_data_reader.h"
#include "perfetto_writer.h"
#include "trace_writer.h"
#include "viz_json_reader.h"

static void usage(const char *prog) {
    fmt::print(stderr,
        "Usage: {} [options]\n"
        "\n"
        "Options:\n"
        "  --perf <path>          Path to perf.data file\n"
        "  --viz <path>           Path to VizTracer JSON file\n"
        "  -o, --output <path>    Output file (default: auto based on format)\n"
        "  --format <fmt>         Output format: json (default) or perfetto\n"
        "  --time-offset <us>     Manual time offset in microseconds\n"
        "  --filter-pid <pid>     Only include events for this PID\n"
        "  --no-sched             Omit scheduler events\n"
        "  --no-gil               Omit GIL tracking events\n"
        "  --no-gpu               Omit GPU/NCCL events\n"
        "  -v, --verbose          Print progress information\n"
        "  -h, --help             Show this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    std::string perf_path;
    std::string viz_path;
    std::string output_path;
    std::string format = "json";
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
            viz_path = next();
        } else if (arg == "-o" || arg == "--output") {
            output_path = next();
        } else if (arg == "--format") {
            format = next();
            if (format != "json" && format != "perfetto") {
                fmt::print(stderr, "Error: --format must be 'json' or 'perfetto'\n");
                return 1;
            }
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

    if (perf_path.empty() && viz_path.empty()) {
        fmt::print(stderr, "Error: at least one of --perf or --viz is required\n");
        usage(argv[0]);
        return 1;
    }

    // Default output filename based on format
    if (output_path.empty()) {
        output_path = (format == "perfetto") ? "merged.perfetto-trace" : "merged.json";
    }

    // Clock aligner
    ClockAligner aligner;
    if (has_time_offset) {
        aligner.set_manual_offset(time_offset);
    }

    // Read perf events
    std::vector<PerfEvent> perf_events;
    std::unordered_map<int32_t, std::string> comm_map;

    if (!perf_path.empty()) {
        if (opts.verbose) {
            fmt::print(stderr, "Reading perf data from {}\n", perf_path);
        }

        try {
            PerfDataReader reader(perf_path);
            reader.read_all_events([&](const PerfEvent &event) {
                perf_events.push_back(event);
            });
            comm_map = reader.comm_map();

            if (opts.verbose) {
                fmt::print(stderr, "Read {} perf events ({} event types)\n",
                           reader.event_count(), reader.event_names().size());
                for (size_t i = 0; i < reader.event_names().size(); i++) {
                    if (!reader.event_names()[i].empty()) {
                        fmt::print(stderr, "  Event {}: {}\n", i, reader.event_names()[i]);
                    }
                }
            }
        } catch (const std::exception &e) {
            fmt::print(stderr, "Error reading perf data: {}\n", e.what());
            return 1;
        }
    }

    // Read VizTracer events
    std::vector<VizEvent> viz_events;

    if (!viz_path.empty()) {
        if (opts.verbose) {
            fmt::print(stderr, "Reading VizTracer data from {}\n", viz_path);
        }

        try {
            VizJsonReader reader(viz_path);
            reader.read_all_events([&](const VizEvent &event) {
                viz_events.push_back(event);
            });

            if (opts.verbose) {
                fmt::print(stderr, "Read {} VizTracer events\n", reader.event_count());
            }
        } catch (const std::exception &e) {
            fmt::print(stderr, "Error reading VizTracer data: {}\n", e.what());
            return 1;
        }
    }

    // Auto-detect clock alignment if both sources are present
    if (!perf_events.empty() && !viz_events.empty() && !has_time_offset) {
        uint64_t perf_first = perf_events.front().timestamp_ns;
        uint64_t perf_last = perf_events.front().timestamp_ns;
        for (const auto &e : perf_events) {
            if (e.timestamp_ns < perf_first) perf_first = e.timestamp_ns;
            if (e.timestamp_ns > perf_last) perf_last = e.timestamp_ns;
        }

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
        fmt::print(stderr, "Writing {} output to {}\n", format, output_path);
    }

    try {
        std::unique_ptr<OutputWriter> writer;
        if (format == "perfetto") {
            writer = std::make_unique<PerfettoWriter>(output_path);
        } else {
            writer = std::make_unique<TraceWriter>(output_path);
        }

        MergeEngine engine(*writer, aligner, opts);

        if (!perf_events.empty()) {
            engine.add_perf_events(std::move(perf_events), comm_map);
        }

        if (!perf_path.empty() && !viz_path.empty()) {
            engine.merge_viz_events(viz_events);
        } else if (!perf_path.empty()) {
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
