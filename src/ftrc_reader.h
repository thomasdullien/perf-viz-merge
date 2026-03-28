#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "event_types.h"

struct ftrc_reader;

class FtrcReader {
public:
    explicit FtrcReader(const std::string &path);
    ~FtrcReader();
    FtrcReader(const FtrcReader &) = delete;
    FtrcReader &operator=(const FtrcReader &) = delete;
    FtrcReader(FtrcReader &&other) noexcept;
    FtrcReader &operator=(FtrcReader &&other) noexcept;

    // Callback-based: read all events in one pass (original API)
    using EventCallback = std::function<void(const VizEvent &)>;
    void read_all_events(EventCallback cb);

    // Pull-based iteration: open file, then call has_next/peek/advance.
    // Events are yielded in libftrc order (sorted by timestamp within file).
    void begin_iteration();
    bool has_next() const { return has_current_; }
    const VizEvent &peek() const { return current_; }
    void advance();

    uint64_t event_count() const { return event_count_; }
    const std::string &path() const { return path_; }

private:
    std::string path_;
    uint64_t event_count_ = 0;
    struct ftrc_reader *reader_ = nullptr;

    // Pull-based iteration state
    VizEvent current_;
    bool has_current_ = false;
    bool iteration_started_ = false;
};
