#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "event_types.h"
#include "perf_data_format.h"

// Reads a perf.data file and yields decoded PerfEvents.
// Uses mmap for efficient access and libtraceevent for tracepoint decoding.

struct tep_handle;

class PerfDataReader {
public:
    explicit PerfDataReader(const std::string &path);
    ~PerfDataReader();

    // Non-copyable
    PerfDataReader(const PerfDataReader &) = delete;
    PerfDataReader &operator=(const PerfDataReader &) = delete;

    // Read all events, calling cb for each decoded event.
    // Events are yielded in file order (not necessarily time-sorted).
    using EventCallback = std::function<void(const PerfEvent &)>;
    void read_all_events(EventCallback cb);

    // Pull-based iteration: call next_event() repeatedly until it returns false.
    // This allows interleaving reads with processing (e.g., for streaming sort).
    bool next_event(PerfEvent &out);

    // Initialize the pull-based cursor (must be called before next_event).
    void begin_iteration();

    // File size accessor
    size_t file_size() const { return mmap_size_; }

    // Metadata accessors
    uint64_t event_count() const { return event_count_; }
    const std::vector<std::string> &event_names() const { return attr_names_; }

    // PID/TID to comm name mapping (populated during read)
    const std::unordered_map<int32_t, std::string> &comm_map() const { return comm_map_; }

private:
    int fd_ = -1;
    uint8_t *mmap_base_ = nullptr;
    size_t mmap_size_ = 0;

    perf_format::FileHeader header_;
    std::vector<perf_format::EventAttr> attrs_;
    std::vector<std::string> attr_names_;
    std::vector<std::vector<uint64_t>> attr_ids_; // per-attr event IDs

    // libtraceevent handle
    struct tep_handle *tep_ = nullptr;

    // ID -> attr index mapping
    std::unordered_map<uint64_t, size_t> id_to_attr_;

    // PID/TID -> comm name
    std::unordered_map<int32_t, std::string> comm_map_;

    uint64_t event_count_ = 0;

    // Pull-based iteration cursor
    const uint8_t *cursor_ = nullptr;
    const uint8_t *cursor_end_ = nullptr;

    void parse_header();
    void parse_attrs();
    void parse_feature_sections();
    void parse_event_desc_section(const uint8_t *data, size_t size);
    void parse_data_section(EventCallback &cb);

    // Decode a PERF_RECORD_SAMPLE into a PerfEvent
    bool decode_sample(const uint8_t *payload, size_t payload_size,
                       const perf_format::EventAttr &attr,
                       PerfEvent &out);

    // Decode a PERF_RECORD_COMM
    void decode_comm(const uint8_t *payload, size_t payload_size);

    // Decode a PERF_RECORD_FORK
    bool decode_fork(const uint8_t *payload, size_t payload_size, PerfEvent &out);

    // Find the attr index for a given event, using sample_id at the end of record
    size_t find_attr_index(const uint8_t *record, size_t record_size);

    // Classify a sample event based on its attr/tracepoint name
    PerfEventType classify_event(size_t attr_index) const;
};
