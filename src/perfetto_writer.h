#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "output_writer.h"

// Hand-encoded protobuf writer for Perfetto's native trace format.
// No protobuf library dependency — encodes wire format directly.

// Low-level protobuf encoding into a byte buffer.
class ProtoEncoder {
public:
    void write_varint(uint64_t value) {
        while (value > 0x7f) {
            buf_.push_back(static_cast<char>((value & 0x7f) | 0x80));
            value >>= 7;
        }
        buf_.push_back(static_cast<char>(value));
    }

    void write_tag(uint32_t field, uint32_t wire_type) {
        write_varint((static_cast<uint64_t>(field) << 3) | wire_type);
    }

    void write_uint64(uint32_t field, uint64_t value) {
        write_tag(field, 0);
        write_varint(value);
    }

    void write_int64(uint32_t field, int64_t value) {
        write_tag(field, 0);
        write_varint(static_cast<uint64_t>(value));
    }

    void write_string(uint32_t field, std::string_view s) {
        write_tag(field, 2);
        write_varint(s.size());
        buf_.append(s);
    }

    void write_bytes(uint32_t field, const std::string &bytes) {
        write_tag(field, 2);
        write_varint(bytes.size());
        buf_.append(bytes);
    }

    template <typename F>
    void write_nested(uint32_t field, F &&fn) {
        ProtoEncoder sub;
        fn(sub);
        write_bytes(field, sub.data());
    }

    void buf_push(char byte) { buf_.push_back(byte); }

    const std::string &data() const { return buf_; }
    size_t size() const { return buf_.size(); }
    void clear() { buf_.clear(); }

private:
    std::string buf_;
};

class PerfettoWriter : public OutputWriter {
public:
    explicit PerfettoWriter(const std::string &path);
    ~PerfettoWriter() override;

    void write_complete(std::string_view name, std::string_view cat,
                        double ts_us, double dur_us,
                        int64_t pid, int64_t tid,
                        std::string_view args_json = "{}") override;

    void write_begin(std::string_view name, std::string_view cat,
                     double ts_us, int64_t pid, int64_t tid,
                     std::string_view args_json = "{}") override;

    void write_end(std::string_view name, std::string_view cat,
                   double ts_us, int64_t pid, int64_t tid,
                   std::string_view args_json = "{}") override;

    void write_instant(std::string_view name, std::string_view cat,
                       double ts_us, int64_t pid, int64_t tid,
                       std::string_view scope = "t",
                       std::string_view args_json = "{}") override;

    void write_viz_event(char ph, std::string_view name, std::string_view cat,
                         double ts_us, double dur_us,
                         int64_t pid, int64_t tid,
                         std::string_view args_json = "{}") override;

    void write_metadata(std::string_view name_key, int64_t pid, int64_t tid,
                        std::string_view args_json) override;

    // Write a counter (time-series) value on a named counter track.
    // Creates the counter track descriptor on first use.
    void write_counter(std::string_view metric_name, double ts_us,
                       double value, int64_t pid);

    uint64_t events_written() const override { return count_; }

    void finalize() override;

private:
    FILE *out_ = nullptr;
    uint64_t count_ = 0;
    bool finalized_ = false;

    // Trusted packet sequence ID (all packets share one sequence)
    static constexpr uint32_t SEQ_ID = 1;
    bool state_cleared_ = false;

    // String interning
    std::unordered_map<std::string, uint64_t> event_name_iids_;
    std::unordered_map<std::string, uint64_t> category_iids_;
    uint64_t next_name_iid_ = 1;
    uint64_t next_cat_iid_ = 1;

    // Track management
    std::unordered_set<uint64_t> defined_tracks_;

    // Synthetic TID offsets (must match merge_engine.h)
    static constexpr int64_t GIL_TID_OFFSET = 100000000;
    static constexpr int64_t SCHED_TID_OFFSET = 200000000;
    static constexpr int64_t GPU_TID_OFFSET = 300000000;
    static constexpr uint64_t COUNTER_UUID_BASE = 500000000ULL;

    // Counter tracks: metric_name → uuid
    std::unordered_map<std::string, uint64_t> counter_tracks_;
    uint64_t next_counter_uuid_ = COUNTER_UUID_BASE;

    // Perfetto protobuf field numbers
    struct TracePacketFields {
        static constexpr uint32_t timestamp = 8;
        static constexpr uint32_t track_event = 11;
        static constexpr uint32_t track_descriptor = 60;
        static constexpr uint32_t trusted_packet_sequence_id = 10;
        static constexpr uint32_t sequence_flags = 13;
        static constexpr uint32_t interned_data = 12;
    };

    struct TrackEventFields {
        static constexpr uint32_t type = 9;
        static constexpr uint32_t track_uuid = 11;
        static constexpr uint32_t name_iid = 10;
        static constexpr uint32_t category_iids = 3;
    };

    struct TrackEventType {
        static constexpr uint64_t SLICE_BEGIN = 1;
        static constexpr uint64_t SLICE_END = 2;
        static constexpr uint64_t INSTANT = 3;
        static constexpr uint64_t COUNTER = 4;
    };

    // TrackEvent field for double counter values
    static constexpr uint32_t kTrackEventDoubleCounterValue = 44;

    // TrackDescriptor field for counter descriptor
    struct CounterDescriptorFields {
        static constexpr uint32_t counter = 8;  // TrackDescriptor.counter
        static constexpr uint32_t unit = 3;     // CounterDescriptor.unit
    };

    struct TrackDescriptorFields {
        static constexpr uint32_t uuid = 1;
        static constexpr uint32_t parent_uuid = 5;
        static constexpr uint32_t name = 2;
        static constexpr uint32_t process = 3;
        static constexpr uint32_t thread = 4;
        static constexpr uint32_t child_ordering = 11;
        static constexpr uint32_t sibling_order_rank = 12;
    };

    struct ProcessDescriptorFields {
        static constexpr uint32_t pid = 1;
        static constexpr uint32_t process_name = 6;
    };

    struct ThreadDescriptorFields {
        static constexpr uint32_t pid = 1;
        static constexpr uint32_t tid = 2;
        static constexpr uint32_t thread_name = 5;
    };

    struct InternedDataFields {
        static constexpr uint32_t event_categories = 1;
        static constexpr uint32_t event_names = 2;
    };

    struct InternedStringFields {
        static constexpr uint32_t iid = 1;
        static constexpr uint32_t name = 2;
    };

    // child_ordering enum values
    static constexpr uint64_t CHILD_ORDERING_EXPLICIT = 1;

    // Write a raw TracePacket to the file (wrapped in Trace.packet field 1)
    void write_packet(const std::string &packet_data);

    // Compute track UUID from pid/tid (including synthetic TID detection)
    uint64_t track_uuid_for(int64_t pid, int64_t tid);

    // Ensure a track descriptor has been emitted for this pid/tid
    void ensure_track(int64_t pid, int64_t tid);

    // Emit a process track descriptor
    void emit_process_track(int64_t pid, std::string_view name);

    // Emit a thread track descriptor
    void emit_thread_track(int64_t pid, int64_t tid, std::string_view name);

    // Emit a child track descriptor (for sched/GIL sub-tracks)
    void emit_child_track(uint64_t uuid, uint64_t parent_uuid,
                          std::string_view name, int32_t sort_rank);

    // Get or create interned ID for an event name; writes interning data to encoder
    uint64_t intern_event_name(const std::string &name, ProtoEncoder &interned);

    // Get or create interned ID for a category; writes interning data to encoder
    uint64_t intern_category(const std::string &cat, ProtoEncoder &interned);

    // Write a track event packet
    void write_track_event(std::string_view name, std::string_view cat,
                           double ts_us, int64_t pid, int64_t tid,
                           uint64_t event_type);

    // Parse a name from metadata args JSON (e.g., {"name":"foo"})
    static std::string parse_name_from_args(std::string_view args_json);
};
