#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "output_writer.h"

// Streaming Chrome Trace Format JSON writer.
// Writes events incrementally to avoid holding the full output in memory.

class TraceWriter : public OutputWriter {
public:
    explicit TraceWriter(const std::string &path);
    ~TraceWriter() override;

    void write_complete(std::string_view name, std::string_view cat,
                        double ts_us, double dur_us,
                        int64_t pid, int64_t tid,
                        std::string_view args_json = "{}") override;

    void write_begin(std::string_view name, std::string_view cat,
                     double ts_us, int64_t pid, int64_t tid,
                     std::string_view args_json = "{}") override;

    void write_end(std::string_view name, std::string_view cat,
                   double ts_us, int64_t pid, int64_t tid,
                   std::string_view args_json = "{}") override;

    void write_instant(std::string_view name, std::string_view cat,
                       double ts_us, int64_t pid, int64_t tid,
                       std::string_view scope = "t",
                       std::string_view args_json = "{}") override;

    void write_viz_event(char ph, std::string_view name, std::string_view cat,
                         double ts_us, double dur_us,
                         int64_t pid, int64_t tid,
                         std::string_view args_json = "{}") override;

    void write_metadata(std::string_view name_key, int64_t pid, int64_t tid,
                        std::string_view args_json) override;

    // Child track methods — Chrome JSON doesn't support child tracks,
    // so just write on the main thread track
    void write_child_complete(ChildTrack type, std::string_view name,
                              std::string_view cat, double ts_us,
                              double dur_us, int64_t pid, int64_t tid) override {
        write_complete(name, cat, ts_us, dur_us, pid, tid);
    }
    void write_child_begin(ChildTrack type, std::string_view name,
                           std::string_view cat, double ts_us,
                           int64_t pid, int64_t tid) override {
        write_begin(name, cat, ts_us, pid, tid);
    }
    void write_child_end(ChildTrack type, std::string_view name,
                         std::string_view cat, double ts_us,
                         int64_t pid, int64_t tid) override {
        write_end(name, cat, ts_us, pid, tid);
    }

    uint64_t events_written() const override { return count_; }

    void finalize() override;

private:
    FILE *out_ = nullptr;
    uint64_t count_ = 0;
    bool finalized_ = false;

    void write_separator();
};
