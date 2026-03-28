#include "ftrc_reader.h"
#include <stdexcept>
#include <fmt/format.h>
extern "C" {
#include "libftrc.h"
}

FtrcReader::FtrcReader(const std::string &path) : path_(path) {}

FtrcReader::~FtrcReader() {
    if (reader_) { ftrc_close(reader_); reader_ = nullptr; }
}

FtrcReader::FtrcReader(FtrcReader &&o) noexcept
    : path_(std::move(o.path_)), event_count_(o.event_count_),
      reader_(o.reader_), current_(std::move(o.current_)),
      has_current_(o.has_current_), iteration_started_(o.iteration_started_) {
    o.reader_ = nullptr;
    o.has_current_ = false;
}

FtrcReader &FtrcReader::operator=(FtrcReader &&o) noexcept {
    if (this != &o) {
        if (reader_) ftrc_close(reader_);
        path_ = std::move(o.path_);
        event_count_ = o.event_count_;
        reader_ = o.reader_;
        current_ = std::move(o.current_);
        has_current_ = o.has_current_;
        iteration_started_ = o.iteration_started_;
        o.reader_ = nullptr;
        o.has_current_ = false;
    }
    return *this;
}

static const char CAT_FEE[] = "FEE";

// Convert a raw ftrc_event to a VizEvent
static VizEvent convert_ftrc_event(const ftrc_event &ev) {
    VizEvent ve;
    ve.pid = ev.pid;
    ve.tid = static_cast<int64_t>(ev.tid);
    ve.depth = ev.depth;
    if (ev.type == FTRC_EVENT_METADATA) {
        ve.ph = 'M';
        ve.ts_us = ev.ts_us;
        ve.dur_us = 0;
        ve.name = std::string(ev.name, ev.name_len);
        std::string_view value(ev.meta_value, ev.meta_value_len);
        ve.args_json = fmt::format(R"({{"name":"{}"}})", value);
    } else {
        ve.ph = 'X';
        ve.ts_us = ev.ts_us;
        ve.dur_us = ev.dur_us;
        ve.name = std::string(ev.name, ev.name_len);
        ve.cat = CAT_FEE;
    }
    return ve;
}

// --- Callback-based API (original) ---

void FtrcReader::read_all_events(EventCallback cb) {
    ftrc_reader *r = ftrc_open(path_.c_str());
    if (!r)
        throw std::runtime_error(fmt::format("Cannot open ftrc file: {}", path_));
    ftrc_event ev;
    while (ftrc_next(r, &ev) == 0) {
        cb(convert_ftrc_event(ev));
        event_count_++;
    }
    reader_ = r;
}

// --- Pull-based iteration API ---

void FtrcReader::begin_iteration() {
    if (reader_) { ftrc_close(reader_); reader_ = nullptr; }
    reader_ = ftrc_open(path_.c_str());
    if (!reader_)
        throw std::runtime_error(fmt::format("Cannot open ftrc file: {}", path_));
    iteration_started_ = true;
    event_count_ = 0;
    advance();  // prime the first event
}

void FtrcReader::advance() {
    if (!reader_) { has_current_ = false; return; }
    ftrc_event ev;
    if (ftrc_next(reader_, &ev) == 0) {
        current_ = convert_ftrc_event(ev);
        has_current_ = true;
        event_count_++;
    } else {
        has_current_ = false;
    }
}
