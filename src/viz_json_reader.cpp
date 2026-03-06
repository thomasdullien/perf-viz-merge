#include "viz_json_reader.h"

#include <simdjson.h>
#include <fmt/format.h>
#include <stdexcept>

using namespace simdjson;

VizJsonReader::VizJsonReader(const std::string &path) : path_(path) {}

VizJsonReader::~VizJsonReader() = default;

void VizJsonReader::read_all_events(EventCallback cb) {
    // simdjson will memory-map the file automatically via padded_string::load
    padded_string json;
    auto load_error = padded_string::load(path_).get(json);
    if (load_error) {
        throw std::runtime_error(
            fmt::format("Cannot load {}: {}", path_, error_message(load_error)));
    }

    ondemand::parser parser;
    ondemand::document doc;
    auto doc_error = parser.iterate(json).get(doc);
    if (doc_error) {
        throw std::runtime_error(
            fmt::format("Cannot parse {}: {}", path_, error_message(doc_error)));
    }

    // Navigate to traceEvents array
    ondemand::array events;
    auto arr_error = doc["traceEvents"].get_array().get(events);
    if (arr_error) {
        throw std::runtime_error(
            fmt::format("Cannot find traceEvents in {}: {}", path_,
                        error_message(arr_error)));
    }

    for (auto element : events) {
        ondemand::object obj;
        if (element.get_object().get(obj)) {
            continue; // Skip non-object elements
        }

        VizEvent event;

        // ph (phase) - required
        std::string_view ph;
        if (obj["ph"].get_string().get(ph)) {
            continue; // Skip events without ph
        }
        if (ph.empty()) continue;
        event.ph = ph[0];

        // ts (timestamp in microseconds) - required
        double ts;
        if (obj["ts"].get_double().get(ts)) {
            continue;
        }
        event.ts_us = ts;

        // pid - required
        int64_t pid;
        if (obj["pid"].get_int64().get(pid)) {
            continue;
        }
        event.pid = pid;

        // tid - required
        int64_t tid;
        if (obj["tid"].get_int64().get(tid)) {
            continue;
        }
        event.tid = tid;

        // name - optional but common
        std::string_view name;
        if (!obj["name"].get_string().get(name)) {
            event.name = std::string(name);
        }

        // cat - optional
        std::string_view cat;
        if (!obj["cat"].get_string().get(cat)) {
            event.cat = std::string(cat);
        }

        // dur - optional (for X events)
        double dur = 0;
        if (!obj["dur"].get_double().get(dur)) {
            event.dur_us = dur;
        }

        // args - optional, store as raw JSON string
        ondemand::object args;
        if (!obj["args"].get_object().get(args)) {
            // Serialize the args object back to JSON string
            std::string_view args_raw = args.raw_json();
            event.args_json = std::string(args_raw);
        }

        event_count_++;
        cb(event);
    }
}
