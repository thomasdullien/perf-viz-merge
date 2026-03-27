#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Child track types for sched/GIL/GPU sub-tracks beneath a thread.
enum class ChildTrack : uint8_t {
    SCHED = 0,
    GIL   = 1,
    GPU   = 2,
};

// Abstract output writer interface. Both Chrome Trace JSON and Perfetto
// native protobuf writers implement this interface.

class OutputWriter {
public:
    virtual ~OutputWriter() = default;

    // Non-copyable
    OutputWriter(const OutputWriter &) = delete;
    OutputWriter &operator=(const OutputWriter &) = delete;

    // Write a complete event (ph="X") on a thread track
    virtual void write_complete(std::string_view name, std::string_view cat,
                                double ts_us, double dur_us,
                                int64_t pid, int64_t tid,
                                std::string_view args_json = "{}") = 0;

    // Write a begin event (ph="B") on a thread track
    virtual void write_begin(std::string_view name, std::string_view cat,
                             double ts_us, int64_t pid, int64_t tid,
                             std::string_view args_json = "{}") = 0;

    // Write an end event (ph="E") on a thread track
    virtual void write_end(std::string_view name, std::string_view cat,
                           double ts_us, int64_t pid, int64_t tid,
                           std::string_view args_json = "{}") = 0;

    // Write events on a child track (sched/GIL/GPU) beneath a thread
    virtual void write_child_complete(ChildTrack type, std::string_view name,
                                      std::string_view cat, double ts_us,
                                      double dur_us, int64_t pid, int64_t tid) = 0;
    virtual void write_child_begin(ChildTrack type, std::string_view name,
                                   std::string_view cat, double ts_us,
                                   int64_t pid, int64_t tid) = 0;
    virtual void write_child_end(ChildTrack type, std::string_view name,
                                 std::string_view cat, double ts_us,
                                 int64_t pid, int64_t tid) = 0;

    // Write an instant event (ph="i")
    virtual void write_instant(std::string_view name, std::string_view cat,
                               double ts_us, int64_t pid, int64_t tid,
                               std::string_view scope = "t",
                               std::string_view args_json = "{}") = 0;

    // Write a raw VizTracer event (pass-through)
    virtual void write_viz_event(char ph, std::string_view name,
                                 std::string_view cat,
                                 double ts_us, double dur_us,
                                 int64_t pid, int64_t tid,
                                 std::string_view args_json = "{}") = 0;

    // Write a metadata event (ph="M") for process/thread naming
    virtual void write_metadata(std::string_view name_key, int64_t pid,
                                int64_t tid, std::string_view args_json) = 0;

    virtual uint64_t events_written() const = 0;

    // Finalize output (called by destructor, but can be called explicitly)
    virtual void finalize() = 0;

protected:
    OutputWriter() = default;
};
