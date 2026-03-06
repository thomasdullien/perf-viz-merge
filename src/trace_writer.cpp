#include "trace_writer.h"

#include <fmt/format.h>
#include <stdexcept>

// Escape a string for JSON output. Handles common special characters.
static std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    return out;
}

TraceWriter::TraceWriter(const std::string &path) {
    out_ = fopen(path.c_str(), "w");
    if (!out_) {
        throw std::runtime_error(
            fmt::format("Cannot open {} for writing", path));
    }
    // Start the JSON
    fmt::print(out_, "{{\"traceEvents\":[\n");
}

TraceWriter::~TraceWriter() {
    finalize();
}

void TraceWriter::finalize() {
    if (finalized_ || !out_) return;
    finalized_ = true;
    fmt::print(out_, "\n]}}\n");
    fclose(out_);
    out_ = nullptr;
}

void TraceWriter::write_separator() {
    if (count_ > 0) {
        fmt::print(out_, ",\n");
    }
    count_++;
}

void TraceWriter::write_complete(std::string_view name, std::string_view cat,
                                 double ts_us, double dur_us,
                                 int64_t pid, int64_t tid,
                                 std::string_view args_json) {
    write_separator();
    fmt::print(out_,
        R"({{"ph":"X","name":"{}","cat":"{}","ts":{:.3f},"dur":{:.3f},"pid":{},"tid":{},"args":{}}})",
        json_escape(name), json_escape(cat), ts_us, dur_us, pid, tid, args_json);
}

void TraceWriter::write_begin(std::string_view name, std::string_view cat,
                              double ts_us, int64_t pid, int64_t tid,
                              std::string_view args_json) {
    write_separator();
    fmt::print(out_,
        R"({{"ph":"B","name":"{}","cat":"{}","ts":{:.3f},"pid":{},"tid":{},"args":{}}})",
        json_escape(name), json_escape(cat), ts_us, pid, tid, args_json);
}

void TraceWriter::write_end(std::string_view name, std::string_view cat,
                            double ts_us, int64_t pid, int64_t tid,
                            std::string_view args_json) {
    write_separator();
    fmt::print(out_,
        R"({{"ph":"E","name":"{}","cat":"{}","ts":{:.3f},"pid":{},"tid":{},"args":{}}})",
        json_escape(name), json_escape(cat), ts_us, pid, tid, args_json);
}

void TraceWriter::write_instant(std::string_view name, std::string_view cat,
                                double ts_us, int64_t pid, int64_t tid,
                                std::string_view scope,
                                std::string_view args_json) {
    write_separator();
    fmt::print(out_,
        R"({{"ph":"i","name":"{}","cat":"{}","ts":{:.3f},"pid":{},"tid":{},"s":"{}","args":{}}})",
        json_escape(name), json_escape(cat), ts_us, pid, tid, scope, args_json);
}

void TraceWriter::write_viz_event(char ph, std::string_view name,
                                  std::string_view cat,
                                  double ts_us, double dur_us,
                                  int64_t pid, int64_t tid,
                                  std::string_view args_json) {
    write_separator();

    if (ph == 'X' && dur_us > 0) {
        fmt::print(out_,
            R"({{"ph":"X","name":"{}","cat":"{}","ts":{:.3f},"dur":{:.3f},"pid":{},"tid":{},"args":{}}})",
            json_escape(name), json_escape(cat), ts_us, dur_us, pid, tid, args_json);
    } else {
        char ph_str[2] = {ph, '\0'};
        fmt::print(out_,
            R"({{"ph":"{}","name":"{}","cat":"{}","ts":{:.3f},"pid":{},"tid":{},"args":{}}})",
            ph_str, json_escape(name), json_escape(cat), ts_us, pid, tid, args_json);
    }
}

void TraceWriter::write_metadata(std::string_view name_key, int64_t pid,
                                 int64_t tid, std::string_view args_json) {
    write_separator();
    fmt::print(out_,
        R"({{"ph":"M","name":"{}","ts":0,"pid":{},"tid":{},"args":{}}})",
        json_escape(name_key), pid, tid, args_json);
}
