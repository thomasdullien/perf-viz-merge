#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "output_writer.h"
#include "perfetto_writer.h"

#include <fmt/format.h>

// OutputWriter adapter that splits output into time-based chunks.
// Wraps PerfettoWriter and swaps it when timestamps cross chunk boundaries.
// Each chunk gets a fresh PerfettoWriter with its own track descriptors.

class ChunkingWriter : public OutputWriter {
public:
    // chunk_duration_us: chunk size in microseconds
    // global_min_us: absolute timestamp of trace start (for computing chunk index)
    // stem, ext: output path components (produces stem-NNN.ext)
    // num_chunks: total number of chunks
    ChunkingWriter(double chunk_duration_us, double global_min_us,
                   std::string stem, std::string ext, int num_chunks,
                   bool verbose = false)
        : chunk_duration_us_(chunk_duration_us),
          global_min_us_(global_min_us),
          stem_(std::move(stem)), ext_(std::move(ext)),
          num_chunks_(num_chunks), verbose_(verbose) {
        // Start with chunk 0
        open_chunk(0);
    }

    ~ChunkingWriter() override {
        finalize();
    }

    // Metadata is forwarded to the current writer. We also cache it
    // so it can be replayed when a new chunk opens.
    void write_metadata(std::string_view name_key, int64_t pid,
                        int64_t tid, std::string_view args_json) override {
        cached_metadata_.push_back({std::string(name_key), pid, tid, std::string(args_json)});
        if (writer_) writer_->write_metadata(name_key, pid, tid, args_json);
    }

    void write_complete(std::string_view name, std::string_view cat,
                        double ts_us, double dur_us,
                        int64_t pid, int64_t tid,
                        std::string_view args_json = "{}") override {
        maybe_advance_chunk(ts_us);
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

    void write_child_complete(ChildTrack type, std::string_view name,
                              std::string_view cat, double ts_us,
                              double dur_us, int64_t pid, int64_t tid) override {
        maybe_advance_chunk(ts_us);
        if (writer_) writer_->write_child_complete(type, name, cat, ts_us, dur_us, pid, tid);
        total_events_++;
    }

    void write_child_begin(ChildTrack type, std::string_view name,
                           std::string_view cat, double ts_us,
                           int64_t pid, int64_t tid) override {
        maybe_advance_chunk(ts_us);
        if (writer_) writer_->write_child_begin(type, name, cat, ts_us, pid, tid);
        total_events_++;
    }

    void write_child_end(ChildTrack type, std::string_view name,
                         std::string_view cat, double ts_us,
                         int64_t pid, int64_t tid) override {
        maybe_advance_chunk(ts_us);
        if (writer_) writer_->write_child_end(type, name, cat, ts_us, pid, tid);
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
        maybe_advance_chunk(ts_us);
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

    // Callback for injecting context spans when a new chunk opens.
    // Called with (chunk_index, chunk_start_us, chunk_end_us, writer).
    using ContextCallback = std::function<void(int chunk_idx, double chunk_start_us,
                                               double chunk_end_us, OutputWriter &writer)>;
    void set_context_callback(ContextCallback cb) { context_cb_ = std::move(cb); }

private:
    void maybe_advance_chunk(double ts_us) {
        int target = chunk_for_ts(ts_us);
        if (target > current_chunk_ && target < num_chunks_) {
            // Close current chunk
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

        // Inject context spans if callback is set
        if (context_cb_) {
            double chunk_start_us = global_min_us_ + idx * chunk_duration_us_;
            double chunk_end_us = global_min_us_ + (idx + 1) * chunk_duration_us_;
            context_cb_(idx, chunk_start_us, chunk_end_us, *writer_);
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
    ContextCallback context_cb_;
};
