#pragma once

#include <functional>
#include <string>

#include "event_types.h"

// Streams VizTracer JSON trace files using simdjson's ondemand API.
// The file is memory-mapped; events are yielded one at a time.

class VizJsonReader {
public:
    explicit VizJsonReader(const std::string &path);
    ~VizJsonReader();

    // Non-copyable
    VizJsonReader(const VizJsonReader &) = delete;
    VizJsonReader &operator=(const VizJsonReader &) = delete;

    // Iterate all trace events, calling cb for each.
    using EventCallback = std::function<void(const VizEvent &)>;
    void read_all_events(EventCallback cb);

    uint64_t event_count() const { return event_count_; }

private:
    std::string path_;
    uint64_t event_count_ = 0;
};
