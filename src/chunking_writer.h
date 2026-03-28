#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "output_writer.h"
#include "perfetto_writer.h"

#include <fmt/format.h>

// OutputWriter adapter that splits output into time-based chunks.
// Wraps PerfettoWriter and swaps it when timestamps cross chunk boundaries.
// Each chunk gets a fresh PerfettoWriter with its own track descriptors.
//
// Maintains a per-thread call stack of open spans so that when a chunk
// boundary is crossed, context spans (the currently-open ancestor frames)
// are emitted into the new chunk, preserving call stack context.

class ChunkingWriter : public OutputWriter {
public:
    ChunkingWriter(double chunk_duration_us, double global_min_us,
                   std::string stem, std::string ext, int num_chunks,
                   bool verbose = false)
        : chunk_duration_us_(chunk_duration_us),
          global_min_us_(global_min_us),
          stem_(std::move(stem)), ext_(std::move(ext)),
          num_chunks_(num_chunks), verbose_(verbose) {
        open_chunk(0);
    }

    ~ChunkingWriter() override {
        finalize();
    }

    void write_metadata(std::string_view name_key, int64_t pid,
                        int64_t tid, std::string_view args_json) override {
        cached_metadata_.push_back({std::string(name_key), pid, tid, std::string(args_json)});
        if (writer_) writer_->write_metadata(name_key, pid, tid, args_json);
    }

    void write_complete(std::string_view name, std::string_view cat,
                        double ts_us, double dur_us,
                        int64_t pid, int64_t tid,
                        std::string_view args_json = "{}") override {
        // Advance chunk first. Expiry happens inside open_chunk at the
        // chunk boundary time, ensuring boundary-crossing spans aren't
        // prematurely removed by a later event's timestamp.
        maybe_advance_chunk(ts_us);
        expire_spans(tid, ts_us);

        // Push this span onto the per-tid stack
        open_stacks_[tid].push_back({
            std::string(name), std::string(cat),
            ts_us, dur_us, pid, tid
        });

        if (writer_) writer_->write_complete(name, cat, ts_us, dur_us, pid, tid, args_json);
        total_events_++;
    }

    void write_begin(std::string_view name, std::string_view cat,
                     double ts_us, int64_t pid, int64_t tid,
                     std::string_view args_json = "{}") override {
        maybe_advance_chunk(ts_us);
        if (writer_) writer_->write_begin(name, cat, ts_us, pid, tid, args_json);
        total_events_++;
    }

    void write_end(std::string_view name, std::string_view cat,
                   double ts_us, int64_t pid, int64_t tid,
                   std::string_view args_json = "{}") override {
        maybe_advance_chunk(ts_us);
        if (writer_) writer_->write_end(name, cat, ts_us, pid, tid, args_json);
        total_events_++;
    }

    void write_instant(std::string_view name, std::string_view cat,
                       double ts_us, int64_t pid, int64_t tid,
                       std::string_view scope = "t",
                       std::string_view args_json = "{}") override {
        maybe_advance_chunk(ts_us);
        if (writer_) writer_->write_instant(name, cat, ts_us, pid, tid, scope, args_json);
        total_events_++;
    }

    void write_viz_event(char ph, std::string_view name,
                         std::string_view cat,
                         double ts_us, double dur_us,
                         int64_t pid, int64_t tid,
                         std::string_view args_json = "{}") override {
        // Advance chunk first, then expire. Order matters: open_chunk
        // expires at the boundary time, preserving boundary-crossing spans
        // for context emission. The event-time expire cleans up after.
        maybe_advance_chunk(ts_us);
        if (ph != 'M') {
            expire_spans(tid, ts_us);
        }

        // For complete events, track in open stacks after chunk advance
        if (ph == 'X' && dur_us > 0) {
            open_stacks_[tid].push_back({
                std::string(name), std::string(cat.empty() ? "python" : cat),
                ts_us, dur_us, pid, tid
            });
        }
        if (writer_) writer_->write_viz_event(ph, name, cat, ts_us, dur_us, pid, tid, args_json);
        total_events_++;
    }

    uint64_t events_written() const override { return total_events_; }

    void finalize() override {
        if (writer_) {
            writer_->finalize();
            if (verbose_) {
                fmt::print(stderr, "Chunk {}: {} events\n",
                           current_chunk_, writer_->events_written());
            }
            writer_.reset();
        }
    }

    int chunks_written() const { return current_chunk_ + 1; }

private:
    // A span that is currently open (started but not yet ended).
    struct OpenSpan {
        std::string name;
        std::string cat;
        double ts_us;
        double dur_us;
        int64_t pid;
        int64_t tid;

        double end_us() const { return ts_us + dur_us; }
    };

    // Remove all spans from the tid's stack that have ended by now_us.
    void expire_spans(int64_t tid, double now_us) {
        auto it = open_stacks_.find(tid);
        if (it == open_stacks_.end()) return;
        auto &stack = it->second;
        // Remove any span that has ended, regardless of position.
        // In a well-nested stack, children end before parents, so
        // popping from the back handles the common case. But sibling
        // spans (same depth, sequential) require a full scan.
        stack.erase(
            std::remove_if(stack.begin(), stack.end(),
                [now_us](const OpenSpan &s) { return s.end_us() <= now_us; }),
            stack.end());
    }

    void maybe_advance_chunk(double ts_us) {
        int target = chunk_for_ts(ts_us);
        if (target > current_chunk_ && target < num_chunks_) {
            if (writer_) {
                writer_->finalize();
                if (verbose_) {
                    fmt::print(stderr, "Chunk {}: {} events\n",
                               current_chunk_, writer_->events_written());
                }
            }
            open_chunk(target);
        }
    }

    int chunk_for_ts(double ts_us) const {
        double offset = ts_us - global_min_us_;
        if (offset < 0) return 0;
        int c = static_cast<int>(offset / chunk_duration_us_);
        if (c >= num_chunks_) c = num_chunks_ - 1;
        return c;
    }

    void open_chunk(int idx) {
        current_chunk_ = idx;
        std::string path = fmt::format("{}-{:03d}{}", stem_, idx, ext_);
        if (verbose_) {
            double start_s = idx * chunk_duration_us_ / 1e6;
            double end_s = (idx + 1) * chunk_duration_us_ / 1e6;
            fmt::print(stderr, "Writing chunk {} ({:.1f}s - {:.1f}s) to {}\n",
                       idx, start_s, end_s, path);
        }
        writer_ = std::make_unique<PerfettoWriter>(path);

        // Replay cached metadata so this chunk has track descriptors
        for (const auto &m : cached_metadata_) {
            writer_->write_metadata(m.name_key, m.pid, m.tid, m.args_json);
        }

        // Emit context spans from per-thread open stacks
        if (idx > 0) {
            double chunk_start_us = global_min_us_ + idx * chunk_duration_us_;
            double chunk_end_us = global_min_us_ + (idx + 1) * chunk_duration_us_;
            int count = 0;

            for (auto &[tid, stack] : open_stacks_) {
                // Expire ended spans first
                expire_spans(tid, chunk_start_us);

                // Emit remaining open spans (shallowest first) as context
                for (const auto &span : stack) {
                    double clamped_dur = span.end_us() - chunk_start_us;
                    if (clamped_dur <= 0) continue;
                    // Cap at chunk end for non-final chunks
                    if (idx < num_chunks_ - 1 && span.end_us() > chunk_end_us) {
                        clamped_dur = chunk_end_us - chunk_start_us;
                    }
                    writer_->write_complete(span.name, span.cat,
                                            chunk_start_us, clamped_dur,
                                            span.pid, span.tid);
                    count++;
                }
            }

            if (verbose_ && count > 0) {
                fmt::print(stderr, "  {} context spans from open stacks\n", count);
            }
        }
    }

    struct CachedMetadata {
        std::string name_key;
        int64_t pid;
        int64_t tid;
        std::string args_json;
    };

    double chunk_duration_us_;
    double global_min_us_;
    std::string stem_;
    std::string ext_;
    int num_chunks_;
    bool verbose_;

    int current_chunk_ = -1;
    std::unique_ptr<PerfettoWriter> writer_;
    std::vector<CachedMetadata> cached_metadata_;
    uint64_t total_events_ = 0;

    // Per-tid stack of currently-open complete (X) spans.
    // Used to emit context spans at chunk boundaries.
    std::unordered_map<int64_t, std::vector<OpenSpan>> open_stacks_;
};
