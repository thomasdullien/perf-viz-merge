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
    using EventCallback = std::function<void(const VizEvent &)>;
    void read_all_events(EventCallback cb);
    uint64_t event_count() const { return event_count_; }
private:
    std::string path_;
    uint64_t event_count_ = 0;
    struct ftrc_reader *reader_ = nullptr;
};
