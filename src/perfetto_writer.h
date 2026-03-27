#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "output_writer.h"

// Distinct type for Perfetto track UUIDs to avoid mixing with raw ints.
enum class TrackUUID : uint64_t {};

inline TrackUUID make_track_uuid(uint64_t v) { return static_cast<TrackUUID>(v); }
inline uint64_t  raw(TrackUUID u)            { return static_cast<uint64_t>(u); }

// Hash support for use in unordered containers.
struct TrackUUIDHash {
    size_t operator()(TrackUUID u) const noexcept {
        return std::hash<uint64_t>{}(raw(u));
    }
};

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

    // Perfetto protobuf field numbers (public for emit_global_track helper)
    struct TracePacketFields {
        static constexpr uint32_t timestamp = 8;
        static constexpr uint32_t track_event = 11;
        static constexpr uint32_t track_descriptor = 60;
        static constexpr uint32_t trusted_packet_sequence_id = 10;
        static constexpr uint32_t sequence_flags = 13;
        static constexpr uint32_t interned_data = 12;
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

    static constexpr uint64_t CHILD_ORDERING_EXPLICIT = 3;

    // Write a raw TracePacket to the file
    void write_packet(const std::string &packet_data);

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
    std::unordered_set<TrackUUID, TrackUUIDHash> defined_tracks_;

    // Deferred track descriptors for process and thread groups.
    // We buffer these so that metadata (real names) can arrive before we emit.
    // Emitted once at finalize().
    struct DeferredTrack {
        TrackUUID uuid;
        TrackUUID parent_uuid;
        std::string fallback_name;  // e.g. "Process 1000"
        int32_t sibling_order_rank = 0;
        bool child_ordering_explicit = false;
    };
    // Key: raw UUID -> deferred descriptor
    std::unordered_map<uint64_t, DeferredTrack> deferred_tracks_;
    // Metadata names: raw UUID -> display name (set by write_metadata)
    std::unordered_map<uint64_t, std::string> metadata_names_;

    // Flush all deferred track descriptors (called from finalize)
    void flush_deferred_tracks();

    // Synthetic TID offsets (must match merge_engine.h)
    static constexpr int64_t GIL_TID_OFFSET = 100000000;
    static constexpr int64_t SCHED_TID_OFFSET = 200000000;
    static constexpr int64_t GPU_TID_OFFSET = 300000000;

    // UUID base offsets for each track type
    static constexpr uint64_t THREAD_UUID_BASE  = 100000000ULL;
    static constexpr uint64_t SCHED_UUID_BASE   = 200000000ULL;
    static constexpr uint64_t GIL_UUID_BASE     = 300000000ULL;
    static constexpr uint64_t GPU_UUID_BASE     = 400000000ULL;
    static constexpr uint64_t COUNTER_UUID_BASE = 500000000ULL;
    static constexpr uint64_t GROUP_UUID_BASE   = 600000000ULL;

    // Counter tracks: metric_name → uuid
    std::unordered_map<std::string, TrackUUID> counter_tracks_;
    uint64_t next_counter_uuid_raw_ = COUNTER_UUID_BASE;

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

    static constexpr uint32_t kTrackEventDoubleCounterValue = 44;

    struct CounterDescriptorFields {
        static constexpr uint32_t counter = 8;
        static constexpr uint32_t unit = 3;
    };

    struct InternedDataFields {
        static constexpr uint32_t event_categories = 1;
        static constexpr uint32_t event_names = 2;
    };

    struct InternedStringFields {
        static constexpr uint32_t iid = 1;
        static constexpr uint32_t name = 2;
    };

    // Compute track UUID from pid/tid (including synthetic TID detection)
    TrackUUID track_uuid_for(int64_t pid, int64_t tid);

    // Ensure a track descriptor has been emitted for this pid/tid
    void ensure_track(int64_t pid, int64_t tid);

    // Ensure the process-level group track exists
    void ensure_process_group(int64_t pid);

    // Ensure the per-thread group track exists (parent of sched/GIL/GPU/stacks)
    void ensure_thread_group(int64_t pid, int64_t real_tid);

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
