#include "ftrc_reader.h"

#include <stdexcept>
#include <fmt/format.h>

extern "C" {
#include "libftrc.h"
}

FtrcReader::FtrcReader(const std::string &path) : path_(path) {}
FtrcReader::~FtrcReader() {}

void FtrcReader::read_all_events(EventCallback cb) {
    ftrc_reader* r = ftrc_open(path_.c_str());
    if (!r) {
        throw std::runtime_error(
            fmt::format("Cannot open ftrc file: {}", path_));
    }

    ftrc_event ev;
    while (ftrc_next(r, &ev) == 0) {
        VizEvent ve;
        ve.pid = ev.pid;
        ve.tid = static_cast<int64_t>(ev.tid);

        if (ev.type == FTRC_EVENT_METADATA) {
            ve.ph = 'M';
            ve.ts_us = ev.ts_us;
            ve.dur_us = 0;
            /* name is "process_name" or "thread_name"; meta_value has the actual name */
            ve.name = std::string(ev.name, ev.name_len);
            std::string_view value(ev.meta_value, ev.meta_value_len);
            ve.args_json = fmt::format(R"({{"name":"{}"}})", value);
        } else {
            ve.ph = 'X';
            ve.ts_us = ev.ts_us;
            ve.dur_us = ev.dur_us;
            ve.name = std::string(ev.name, ev.name_len);
            ve.cat = ev.cat ? ev.cat : "FEE";
        }

        cb(ve);
        event_count_++;
    }

    ftrc_close(r);
}
