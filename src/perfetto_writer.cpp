#include "perfetto_writer.h"

#include <cstring>
#include <stdexcept>
#include <fmt/format.h>

PerfettoWriter::PerfettoWriter(const std::string &path) {
    out_ = fopen(path.c_str(), "wb");
    if (!out_) {
        throw std::runtime_error(
            fmt::format("Cannot open {} for writing", path));
    }
}

PerfettoWriter::~PerfettoWriter() {
    finalize();
}

void PerfettoWriter::finalize() {
    if (finalized_ || !out_) return;
    finalized_ = true;
    fclose(out_);
    out_ = nullptr;
}

void PerfettoWriter::write_packet(const std::string &packet_data) {
    // Trace message: field 1 (TracePacket), wire type 2 (length-delimited)
    ProtoEncoder wrapper;
    wrapper.write_bytes(1, packet_data);
    fwrite(wrapper.data().data(), 1, wrapper.data().size(), out_);
}

uint64_t PerfettoWriter::track_uuid_for(int64_t pid, int64_t tid) {
    // Synthetic TIDs from the merge engine are in range [offset, offset + MAX_REAL_TID).
    // Real Linux TIDs are < ~4M.  Python pthread_t TIDs can be huge (48-bit
    // addresses like 131841036036992) and must NOT be treated as synthetic.
    static constexpr int64_t MAX_REAL_TID = 10000000;  // 10M
    static constexpr int64_t GPU_RANGE_END = GPU_TID_OFFSET + MAX_REAL_TID;
    static constexpr int64_t SCHED_RANGE_END = SCHED_TID_OFFSET + MAX_REAL_TID;
    static constexpr int64_t GIL_RANGE_END = GIL_TID_OFFSET + MAX_REAL_TID;

    if (tid >= GPU_TID_OFFSET && tid < GPU_RANGE_END) {
        // GPU child track: uuid = 400000000 + real_tid
        int64_t real_tid = tid - GPU_TID_OFFSET;
        return 400000000ULL + static_cast<uint64_t>(real_tid);
    } else if (tid >= SCHED_TID_OFFSET && tid < SCHED_RANGE_END) {
        // Sched child track: uuid = 200000000 + real_tid
        return static_cast<uint64_t>(tid);  // tid already = real_tid + 200000000
    } else if (tid >= GIL_TID_OFFSET && tid < GIL_RANGE_END) {
        // GIL child track: uuid = 300000000 + real_tid
        // tid = real_tid + 100000000, we want 300000000 + real_tid
        return static_cast<uint64_t>(tid) + 200000000ULL;
    } else {
        // Thread track: uuid = 100000000 + tid
        return 100000000ULL + static_cast<uint64_t>(tid);
    }
}

void PerfettoWriter::ensure_track(int64_t pid, int64_t tid) {
    uint64_t uuid = track_uuid_for(pid, tid);
    if (defined_tracks_.count(uuid)) return;
    defined_tracks_.insert(uuid);

    // Use same range checks as track_uuid_for
    static constexpr int64_t MAX_REAL_TID = 10000000;
    static constexpr int64_t GPU_RANGE_END = GPU_TID_OFFSET + MAX_REAL_TID;
    static constexpr int64_t SCHED_RANGE_END = SCHED_TID_OFFSET + MAX_REAL_TID;
    static constexpr int64_t GIL_RANGE_END = GIL_TID_OFFSET + MAX_REAL_TID;

    if (tid >= GPU_TID_OFFSET && tid < GPU_RANGE_END) {
        int64_t real_tid = tid - GPU_TID_OFFSET;
        uint64_t parent_uuid = 100000000ULL + static_cast<uint64_t>(real_tid);
        if (!defined_tracks_.count(parent_uuid)) {
            defined_tracks_.insert(parent_uuid);
            emit_thread_track(pid, real_tid, "");
        }
        emit_child_track(uuid, parent_uuid, "GPU", -1);
    } else if (tid >= SCHED_TID_OFFSET && tid < SCHED_RANGE_END) {
        int64_t real_tid = tid - SCHED_TID_OFFSET;
        uint64_t parent_uuid = 100000000ULL + static_cast<uint64_t>(real_tid);
        if (!defined_tracks_.count(parent_uuid)) {
            defined_tracks_.insert(parent_uuid);
            emit_thread_track(pid, real_tid, "");
        }
        emit_child_track(uuid, parent_uuid, "sched", -3);
    } else if (tid >= GIL_TID_OFFSET && tid < GIL_RANGE_END) {
        int64_t real_tid = tid - GIL_TID_OFFSET;
        uint64_t parent_uuid = 100000000ULL + static_cast<uint64_t>(real_tid);
        if (!defined_tracks_.count(parent_uuid)) {
            defined_tracks_.insert(parent_uuid);
            emit_thread_track(pid, real_tid, "");
        }
        emit_child_track(uuid, parent_uuid, "GIL", -2);
    } else {
        // Regular thread track
        emit_thread_track(pid, tid, "");
    }
}

void PerfettoWriter::emit_process_track(int64_t pid, std::string_view name) {
    uint64_t uuid = static_cast<uint64_t>(pid);
    // Skip if already defined, unless we now have a name to update with
    if (defined_tracks_.count(uuid) && name.empty()) return;
    defined_tracks_.insert(uuid);

    ProtoEncoder packet;
    packet.write_nested(TracePacketFields::track_descriptor, [&](ProtoEncoder &td) {
        td.write_uint64(TrackDescriptorFields::uuid, uuid);
        if (!name.empty()) {
            td.write_string(TrackDescriptorFields::name, name);
        }
        td.write_nested(TrackDescriptorFields::process, [&](ProtoEncoder &pd) {
            pd.write_int64(ProcessDescriptorFields::pid, pid);
            if (!name.empty()) {
                pd.write_string(ProcessDescriptorFields::process_name, name);
            }
        });
    });
    write_packet(packet.data());
}

void PerfettoWriter::emit_thread_track(int64_t pid, int64_t tid,
                                        std::string_view name) {
    uint64_t uuid = 100000000ULL + static_cast<uint64_t>(tid);

    // Ensure process track exists
    uint64_t proc_uuid = static_cast<uint64_t>(pid);
    if (!defined_tracks_.count(proc_uuid)) {
        emit_process_track(pid, "");
    }

    ProtoEncoder packet;
    packet.write_nested(TracePacketFields::track_descriptor, [&](ProtoEncoder &td) {
        td.write_uint64(TrackDescriptorFields::uuid, uuid);
        td.write_uint64(TrackDescriptorFields::parent_uuid, proc_uuid);
        if (!name.empty()) {
            td.write_string(TrackDescriptorFields::name, name);
        }
        td.write_nested(TrackDescriptorFields::thread, [&](ProtoEncoder &thd) {
            thd.write_int64(ThreadDescriptorFields::pid, pid);
            thd.write_int64(ThreadDescriptorFields::tid, tid);
            if (!name.empty()) {
                thd.write_string(ThreadDescriptorFields::thread_name, name);
            }
        });
        // Enable explicit child ordering so sched/GIL/callstack are ordered
        td.write_uint64(TrackDescriptorFields::child_ordering, CHILD_ORDERING_EXPLICIT);
    });
    write_packet(packet.data());
}

void PerfettoWriter::emit_child_track(uint64_t uuid, uint64_t parent_uuid,
                                       std::string_view name,
                                       int32_t sort_rank) {
    ProtoEncoder packet;
    packet.write_nested(TracePacketFields::track_descriptor, [&](ProtoEncoder &td) {
        td.write_uint64(TrackDescriptorFields::uuid, uuid);
        td.write_uint64(TrackDescriptorFields::parent_uuid, parent_uuid);
        td.write_string(TrackDescriptorFields::name, name);
        td.write_int64(TrackDescriptorFields::sibling_order_rank, sort_rank);
    });
    write_packet(packet.data());
}

uint64_t PerfettoWriter::intern_event_name(const std::string &name,
                                            ProtoEncoder &interned) {
    auto it = event_name_iids_.find(name);
    if (it != event_name_iids_.end()) {
        return it->second;
    }

    uint64_t iid = next_name_iid_++;
    event_name_iids_[name] = iid;

    // Emit InternedData.event_names entry
    interned.write_nested(InternedDataFields::event_names, [&](ProtoEncoder &en) {
        en.write_uint64(InternedStringFields::iid, iid);
        en.write_string(InternedStringFields::name, name);
    });

    return iid;
}

uint64_t PerfettoWriter::intern_category(const std::string &cat,
                                          ProtoEncoder &interned) {
    auto it = category_iids_.find(cat);
    if (it != category_iids_.end()) {
        return it->second;
    }

    uint64_t iid = next_cat_iid_++;
    category_iids_[cat] = iid;

    // Emit InternedData.event_categories entry
    interned.write_nested(InternedDataFields::event_categories, [&](ProtoEncoder &ec) {
        ec.write_uint64(InternedStringFields::iid, iid);
        ec.write_string(InternedStringFields::name, cat);
    });

    return iid;
}

void PerfettoWriter::write_track_event(std::string_view name,
                                        std::string_view cat, double ts_us,
                                        int64_t pid, int64_t tid,
                                        uint64_t event_type) {
    ensure_track(pid, tid);
    uint64_t uuid = track_uuid_for(pid, tid);

    // Convert microseconds to nanoseconds
    uint64_t ts_ns = static_cast<uint64_t>(ts_us * 1000.0);

    // Build interned data (may be empty if all strings already interned)
    ProtoEncoder interned;
    std::string name_str(name);
    std::string cat_str(cat);
    uint64_t name_iid = intern_event_name(name_str, interned);
    uint64_t cat_iid = intern_category(cat_str, interned);

    ProtoEncoder packet;

    // Sequence flags: clear state on first packet
    packet.write_uint64(TracePacketFields::trusted_packet_sequence_id, SEQ_ID);
    if (!state_cleared_) {
        state_cleared_ = true;
        packet.write_uint64(TracePacketFields::sequence_flags, 1); // SEQ_INCREMENTAL_STATE_CLEARED
    } else {
        packet.write_uint64(TracePacketFields::sequence_flags, 2); // SEQ_NEEDS_INCREMENTAL_STATE
    }

    // Timestamp
    packet.write_uint64(TracePacketFields::timestamp, ts_ns);

    // TrackEvent submessage
    // SLICE_END events omit name_iid/category_iids — trace_processor uses
    // LIFO stack matching for ENDs. Including names would cause spurious
    // "misplaced_end_event" warnings when same-timestamp events get reordered.
    packet.write_nested(TracePacketFields::track_event, [&](ProtoEncoder &te) {
        te.write_uint64(TrackEventFields::type, event_type);
        te.write_uint64(TrackEventFields::track_uuid, uuid);
        if (event_type != TrackEventType::SLICE_END) {
            te.write_uint64(TrackEventFields::name_iid, name_iid);
            te.write_uint64(TrackEventFields::category_iids, cat_iid);
        }
    });

    // Interned data (if any new strings were interned)
    if (interned.size() > 0) {
        packet.write_bytes(TracePacketFields::interned_data, interned.data());
    }

    write_packet(packet.data());
    count_++;
}

void PerfettoWriter::write_complete(std::string_view name, std::string_view cat,
                                     double ts_us, double dur_us,
                                     int64_t pid, int64_t tid,
                                     std::string_view args_json) {
    // Emit as begin + end pair
    write_track_event(name, cat, ts_us, pid, tid, TrackEventType::SLICE_BEGIN);
    // Decrement count since write_track_event incremented it, and we want
    // the pair to count as one logical event (end will increment again)
    count_--;
    write_track_event(name, cat, ts_us + dur_us, pid, tid, TrackEventType::SLICE_END);
}

void PerfettoWriter::write_begin(std::string_view name, std::string_view cat,
                                  double ts_us, int64_t pid, int64_t tid,
                                  std::string_view args_json) {
    write_track_event(name, cat, ts_us, pid, tid, TrackEventType::SLICE_BEGIN);
}

void PerfettoWriter::write_end(std::string_view name, std::string_view cat,
                                double ts_us, int64_t pid, int64_t tid,
                                std::string_view args_json) {
    write_track_event(name, cat, ts_us, pid, tid, TrackEventType::SLICE_END);
}

void PerfettoWriter::write_instant(std::string_view name, std::string_view cat,
                                    double ts_us, int64_t pid, int64_t tid,
                                    std::string_view scope,
                                    std::string_view args_json) {
    write_track_event(name, cat, ts_us, pid, tid, TrackEventType::INSTANT);
}

void PerfettoWriter::write_viz_event(char ph, std::string_view name,
                                      std::string_view cat, double ts_us,
                                      double dur_us, int64_t pid, int64_t tid,
                                      std::string_view args_json) {
    switch (ph) {
    case 'X':
        write_complete(name, cat, ts_us, dur_us, pid, tid, args_json);
        break;
    case 'B':
        write_begin(name, cat, ts_us, pid, tid, args_json);
        break;
    case 'E':
        write_end(name, cat, ts_us, pid, tid, args_json);
        break;
    case 'i':
    case 'I':
        write_instant(name, cat, ts_us, pid, tid, "t", args_json);
        break;
    case 'M':
        write_metadata(name, pid, tid, args_json);
        break;
    default:
        // For unsupported phase types, emit as instant
        write_instant(name, cat, ts_us, pid, tid, "t", args_json);
        break;
    }
}

void PerfettoWriter::write_metadata(std::string_view name_key, int64_t pid,
                                     int64_t tid, std::string_view args_json) {
    std::string name = parse_name_from_args(args_json);

    if (name_key == "process_name") {
        emit_process_track(pid, name);
    } else if (name_key == "thread_name") {
        // Check if this is a synthetic track (sched/GIL/GPU)
        if (tid >= GIL_TID_OFFSET) {
            // Child tracks are created on-demand in ensure_track()
            // Just make sure the process track exists
            emit_process_track(pid, "");
        } else {
            uint64_t uuid = 100000000ULL + static_cast<uint64_t>(tid);
            if (!defined_tracks_.count(uuid)) {
                emit_thread_track(pid, tid, name);
                defined_tracks_.insert(uuid);
            }
        }
    }
    // Ignore thread_sort_index — Perfetto uses sibling_order_rank instead
}

std::string PerfettoWriter::parse_name_from_args(std::string_view args_json) {
    // Simple parser for {"name":"value"} — avoids JSON library dependency
    auto pos = args_json.find("\"name\"");
    if (pos == std::string_view::npos) return "";

    pos = args_json.find('"', pos + 6);
    if (pos == std::string_view::npos) return "";
    pos++; // skip opening quote

    auto end = args_json.find('"', pos);
    if (end == std::string_view::npos) return "";

    return std::string(args_json.substr(pos, end - pos));
}

void PerfettoWriter::write_counter(std::string_view metric_name, double ts_us,
                                    double value, int64_t pid) {
    std::string key(metric_name);
    uint64_t uuid;

    auto it = counter_tracks_.find(key);
    if (it != counter_tracks_.end()) {
        uuid = it->second;
    } else {
        // Create new counter track descriptor
        uuid = next_counter_uuid_++;
        counter_tracks_[key] = uuid;

        // Ensure process track exists
        uint64_t proc_uuid = static_cast<uint64_t>(pid);
        if (!defined_tracks_.count(proc_uuid)) {
            emit_process_track(pid, "");
        }

        // Emit counter track descriptor
        ProtoEncoder packet;
        packet.write_nested(TracePacketFields::track_descriptor, [&](ProtoEncoder &td) {
            td.write_uint64(TrackDescriptorFields::uuid, uuid);
            td.write_uint64(TrackDescriptorFields::parent_uuid, proc_uuid);
            td.write_string(TrackDescriptorFields::name, metric_name);
            // Mark as counter track
            td.write_nested(CounterDescriptorFields::counter, [&](ProtoEncoder &cd) {
                // unit = UNSPECIFIED (0) — Perfetto infers from name
                (void)cd;
            });
        });
        write_packet(packet.data());
    }

    // Emit counter value
    uint64_t ts_ns = static_cast<uint64_t>(ts_us * 1000.0);

    ProtoEncoder packet;
    packet.write_uint64(TracePacketFields::trusted_packet_sequence_id, SEQ_ID);
    if (!state_cleared_) {
        state_cleared_ = true;
        packet.write_uint64(TracePacketFields::sequence_flags, 1);
    } else {
        packet.write_uint64(TracePacketFields::sequence_flags, 2);
    }
    packet.write_uint64(TracePacketFields::timestamp, ts_ns);
    packet.write_nested(TracePacketFields::track_event, [&](ProtoEncoder &te) {
        te.write_uint64(TrackEventFields::type, TrackEventType::COUNTER);
        te.write_uint64(TrackEventFields::track_uuid, uuid);
        // double_counter_value is field 44, wire type 1 (64-bit fixed)
        te.write_tag(kTrackEventDoubleCounterValue, 1);  // wire type 1 = fixed64
        uint64_t bits;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        // Write raw 8 bytes (little-endian fixed64)
        for (int i = 0; i < 8; i++) {
            te.buf_push(static_cast<char>(bits & 0xff));
            bits >>= 8;
        }
    });
    write_packet(packet.data());
    count_++;
}
