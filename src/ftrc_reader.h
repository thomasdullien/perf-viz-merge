#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "event_types.h"

// Reads fasttracer .ftrc binary trace files directly, converting
// entry/exit event pairs into VizEvent "X" (complete) events.
//
// This replaces the ftrc → JSON → simdjson pipeline, avoiding the
// 17x size expansion and simdjson's ~4GB file size limit.
//
// Supports format v2 (12-byte events, uint32 func_id, exit in flags bit 7).

class FtrcReader {
public:
    explicit FtrcReader(const std::string &path);
    ~FtrcReader();

    // Non-copyable
    FtrcReader(const FtrcReader &) = delete;
    FtrcReader &operator=(const FtrcReader &) = delete;

    // Iterate all trace events, calling cb for each completed event.
    // Entry/exit pairs are matched and emitted as VizEvent with ph='X'.
    using EventCallback = std::function<void(const VizEvent &)>;
    void read_all_events(EventCallback cb);

    uint64_t event_count() const { return event_count_; }

private:
    std::string path_;
    uint64_t event_count_ = 0;
};
