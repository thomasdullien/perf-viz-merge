#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

// Streaming Chrome Trace Format JSON writer.
// Writes events incrementally to avoid holding the full output in memory.

class TraceWriter {
public:
    explicit TraceWriter(const std::string &path);
    ~TraceWriter();

    // Non-copyable
    TraceWriter(const TraceWriter &) = delete;
    TraceWriter &operator=(const TraceWriter &) = delete;

    // Write a complete event (ph="X")
    void write_complete(std::string_view name, std::string_view cat,
                        double ts_us, double dur_us,
                        int64_t pid, int64_t tid,
                        std::string_view args_json = "{}");

    // Write a begin event (ph="B")
    void write_begin(std::string_view name, std::string_view cat,
                     double ts_us, int64_t pid, int64_t tid,
                     std::string_view args_json = "{}");

    // Write an end event (ph="E")
    void write_end(std::string_view name, std::string_view cat,
                   double ts_us, int64_t pid, int64_t tid,
                   std::string_view args_json = "{}");

    // Write an instant event (ph="i")
    void write_instant(std::string_view name, std::string_view cat,
                       double ts_us, int64_t pid, int64_t tid,
                       std::string_view scope = "t",
                       std::string_view args_json = "{}");

    // Write a raw VizTracer event (pass-through)
    void write_viz_event(char ph, std::string_view name, std::string_view cat,
                         double ts_us, double dur_us,
                         int64_t pid, int64_t tid,
                         std::string_view args_json = "{}");

    // Write a metadata event (ph="M") for process/thread naming
    void write_metadata(std::string_view name_key, int64_t pid, int64_t tid,
                        std::string_view args_json);

    uint64_t events_written() const { return count_; }

    // Finalize (called by destructor, but can be called explicitly)
    void finalize();

private:
    FILE *out_ = nullptr;
    uint64_t count_ = 0;
    bool finalized_ = false;

    void write_separator();
};
