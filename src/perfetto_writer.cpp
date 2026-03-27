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

void PerfettoWriter::write_packet(const std::string &packet_data) {
    ProtoEncoder wrapper;
    wrapper.write_bytes(1, packet_data);
    fwrite(wrapper.data().data(), 1, wrapper.data().size(), out_);
}

// --- UUID computation ---

TrackUUID PerfettoWriter::track_uuid_for(int64_t pid, int64_t tid) {
    static constexpr int64_t MAX_REAL_TID = 10000000;
    static constexpr int64_t GPU_RANGE_END = GPU_TID_OFFSET + MAX_REAL_TID;
    static constexpr int64_t SCHED_RANGE_END = SCHED_TID_OFFSET + MAX_REAL_TID;
    static constexpr int64_t GIL_RANGE_END = GIL_TID_OFFSET + MAX_REAL_TID;

    if (tid >= GPU_TID_OFFSET && tid < GPU_RANGE_END) {
        int64_t real_tid = tid - GPU_TID_OFFSET;
        return make_track_uuid(GPU_UUID_BASE + static_cast<uint64_t>(real_tid));
    } else if (tid >= SCHED_TID_OFFSET && tid < SCHED_RANGE_END) {
        int64_t real_tid = tid - SCHED_TID_OFFSET;
        return make_track_uuid(SCHED_UUID_BASE + static_cast<uint64_t>(real_tid));
    } else if (tid >= GIL_TID_OFFSET && tid < GIL_RANGE_END) {
        int64_t real_tid = tid - GIL_TID_OFFSET;
        return make_track_uuid(GIL_UUID_BASE + static_cast<uint64_t>(real_tid));
    } else {
        // Call stack track
        return make_track_uuid(THREAD_UUID_BASE + static_cast<uint64_t>(tid));
    }
}

// --- Emit helpers for all-global track hierarchy ---

// Emit a global track (no ThreadDescriptor, no ProcessDescriptor).
// All tracks in our hierarchy are global so parent-child nesting works.
static void emit_global_track(PerfettoWriter &w,
                               TrackUUID uuid,
                               TrackUUID parent_uuid,
                               std::string_view name,
                               int32_t sibling_order_rank,
                               bool explicit_child_ordering) {
    ProtoEncoder packet;
    packet.write_nested(PerfettoWriter::TracePacketFields::track_descriptor,
                        [&](ProtoEncoder &td) {
        td.write_uint64(PerfettoWriter::TrackDescriptorFields::uuid, raw(uuid));
        if (raw(parent_uuid) != 0) {
            td.write_uint64(PerfettoWriter::TrackDescriptorFields::parent_uuid,
                            raw(parent_uuid));
        }
        if (!name.empty()) {
            td.write_string(PerfettoWriter::TrackDescriptorFields::name, name);
        }
        if (sibling_order_rank != 0) {
            td.write_int64(
                PerfettoWriter::TrackDescriptorFields::sibling_order_rank,
                sibling_order_rank);
        }
        if (explicit_child_ordering) {
            td.write_uint64(
                PerfettoWriter::TrackDescriptorFields::child_ordering,
                PerfettoWriter::CHILD_ORDERING_EXPLICIT);
        }
    });
    w.write_packet(packet.data());
}

// --- Deferred track emission ---

void PerfettoWriter::flush_deferred_tracks() {
    for (auto &[uuid_raw, dt] : deferred_tracks_) {
        // Use metadata name if available, otherwise fall back to placeholder
        auto it = metadata_names_.find(uuid_raw);
        const std::string &name = (it != metadata_names_.end())
                                      ? it->second
                                      : dt.fallback_name;
        emit_global_track(*this, dt.uuid, dt.parent_uuid, name,
                          dt.sibling_order_rank, dt.child_ordering_explicit);
    }
    deferred_tracks_.clear();
    metadata_names_.clear();
}

void PerfettoWriter::finalize() {
    if (finalized_ || !out_) return;
    flush_deferred_tracks();
    finalized_ = true;
    fclose(out_);
    out_ = nullptr;
}

// --- Track management ---

void PerfettoWriter::ensure_process_group(int64_t pid) {
    TrackUUID proc_uuid = make_track_uuid(static_cast<uint64_t>(pid));
    if (defined_tracks_.count(proc_uuid)) return;
    defined_tracks_.insert(proc_uuid);

    // Defer emission — real name may arrive later via write_metadata.
    DeferredTrack dt;
    dt.uuid = proc_uuid;
    dt.parent_uuid = make_track_uuid(0);
    dt.fallback_name = fmt::format("Process {}", pid);
    dt.sibling_order_rank = 0;
    dt.child_ordering_explicit = true;
    deferred_tracks_[raw(proc_uuid)] = std::move(dt);
}

void PerfettoWriter::ensure_thread_group(int64_t pid, int64_t real_tid) {
    TrackUUID group_uuid = make_track_uuid(
        GROUP_UUID_BASE + static_cast<uint64_t>(real_tid));
    if (defined_tracks_.count(group_uuid)) return;
    defined_tracks_.insert(group_uuid);

    ensure_process_group(pid);
    TrackUUID proc_uuid = make_track_uuid(static_cast<uint64_t>(pid));

    // Defer emission — real name may arrive later via write_metadata.
    DeferredTrack dt;
    dt.uuid = group_uuid;
    dt.parent_uuid = proc_uuid;
    dt.fallback_name = fmt::format("Thread {}", real_tid);
    dt.sibling_order_rank = 0;
    dt.child_ordering_explicit = true;
    deferred_tracks_[raw(group_uuid)] = std::move(dt);
}

void PerfettoWriter::ensure_track(int64_t pid, int64_t tid) {
    TrackUUID uuid = track_uuid_for(pid, tid);
    if (defined_tracks_.count(uuid)) return;
    defined_tracks_.insert(uuid);

    static constexpr int64_t MAX_REAL_TID = 10000000;
    static constexpr int64_t GPU_RANGE_END = GPU_TID_OFFSET + MAX_REAL_TID;
    static constexpr int64_t SCHED_RANGE_END = SCHED_TID_OFFSET + MAX_REAL_TID;
    static constexpr int64_t GIL_RANGE_END = GIL_TID_OFFSET + MAX_REAL_TID;

    if (tid >= GPU_TID_OFFSET && tid < GPU_RANGE_END) {
        int64_t real_tid = tid - GPU_TID_OFFSET;
        ensure_thread_group(pid, real_tid);
        TrackUUID parent = make_track_uuid(
            GROUP_UUID_BASE + static_cast<uint64_t>(real_tid));
        std::string name = fmt::format("GPU [{}]", real_tid);
        emit_global_track(*this, uuid, parent, name, -25, false);
    } else if (tid >= SCHED_TID_OFFSET && tid < SCHED_RANGE_END) {
        int64_t real_tid = tid - SCHED_TID_OFFSET;
        ensure_thread_group(pid, real_tid);
        TrackUUID parent = make_track_uuid(
            GROUP_UUID_BASE + static_cast<uint64_t>(real_tid));
        std::string name = fmt::format("sched [{}]", real_tid);
        emit_global_track(*this, uuid, parent, name, -100, false);
    } else if (tid >= GIL_TID_OFFSET && tid < GIL_RANGE_END) {
        int64_t real_tid = tid - GIL_TID_OFFSET;
        ensure_thread_group(pid, real_tid);
        TrackUUID parent = make_track_uuid(
            GROUP_UUID_BASE + static_cast<uint64_t>(real_tid));
        std::string name = fmt::format("GIL [{}]", real_tid);
        emit_global_track(*this, uuid, parent, name, -50, false);
    } else {
        // Call stack track — child of thread group
        ensure_thread_group(pid, tid);
        TrackUUID parent = make_track_uuid(
            GROUP_UUID_BASE + static_cast<uint64_t>(tid));
        std::string name = fmt::format("stacks [{}]", tid);
        emit_global_track(*this, uuid, parent, name, 0, false);
    }
}

// --- Metadata (naming tracks) ---

void PerfettoWriter::write_metadata(std::string_view name_key, int64_t pid,
                                     int64_t tid, std::string_view args_json) {
    std::string name = parse_name_from_args(args_json);

    if (name_key == "process_name") {
        ensure_process_group(pid);
        TrackUUID proc_uuid = make_track_uuid(static_cast<uint64_t>(pid));
        // Store display name for deferred emission at finalize
        metadata_names_[raw(proc_uuid)] = fmt::format("{} [{}]", name, pid);
    } else if (name_key == "thread_name") {
        if (tid >= GIL_TID_OFFSET) {
            ensure_process_group(pid);
        } else {
            ensure_thread_group(pid, tid);
            TrackUUID group_uuid = make_track_uuid(
                GROUP_UUID_BASE + static_cast<uint64_t>(tid));
            // Store display name for deferred emission at finalize
            metadata_names_[raw(group_uuid)] =
                fmt::format("{} [{}]", name, tid);
        }
    }
}

std::string PerfettoWriter::parse_name_from_args(std::string_view args_json) {
    auto pos = args_json.find("\"name\"");
    if (pos == std::string_view::npos) return "";
    pos = args_json.find('"', pos + 6);
    if (pos == std::string_view::npos) return "";
    pos++;
    auto end = args_json.find('"', pos);
    if (end == std::string_view::npos) return "";
    return std::string(args_json.substr(pos, end - pos));
}

// --- String interning ---

uint64_t PerfettoWriter::intern_event_name(const std::string &name,
                                            ProtoEncoder &interned) {
    auto it = event_name_iids_.find(name);
    if (it != event_name_iids_.end()) return it->second;

    uint64_t iid = next_name_iid_++;
    event_name_iids_[name] = iid;

    interned.write_nested(InternedDataFields::event_names, [&](ProtoEncoder &en) {
        en.write_uint64(InternedStringFields::iid, iid);
        en.write_string(InternedStringFields::name, name);
    });
    return iid;
}

uint64_t PerfettoWriter::intern_category(const std::string &cat,
                                          ProtoEncoder &interned) {
    auto it = category_iids_.find(cat);
    if (it != category_iids_.end()) return it->second;

    uint64_t iid = next_cat_iid_++;
    category_iids_[cat] = iid;

    interned.write_nested(InternedDataFields::event_categories, [&](ProtoEncoder &ec) {
        ec.write_uint64(InternedStringFields::iid, iid);
        ec.write_string(InternedStringFields::name, cat);
    });
    return iid;
}

// --- Core event writing ---

void PerfettoWriter::write_track_event(std::string_view name,
                                        std::string_view cat, double ts_us,
                                        int64_t pid, int64_t tid,
                                        uint64_t event_type) {
    ensure_track(pid, tid);
    TrackUUID uuid = track_uuid_for(pid, tid);
    uint64_t ts_ns = static_cast<uint64_t>(ts_us * 1000.0);

    ProtoEncoder interned;
    std::string name_str(name);
    std::string cat_str(cat);
    uint64_t name_iid = intern_event_name(name_str, interned);
    uint64_t cat_iid = intern_category(cat_str, interned);

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
        te.write_uint64(TrackEventFields::type, event_type);
        te.write_uint64(TrackEventFields::track_uuid, raw(uuid));
        if (event_type != TrackEventType::SLICE_END) {
            te.write_uint64(TrackEventFields::name_iid, name_iid);
            te.write_uint64(TrackEventFields::category_iids, cat_iid);
        }
    });

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
    write_track_event(name, cat, ts_us, pid, tid, TrackEventType::SLICE_BEGIN);
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
        write_instant(name, cat, ts_us, pid, tid, "t", args_json);
        break;
    }
}

// --- Counter tracks ---

void PerfettoWriter::write_counter(std::string_view metric_name, double ts_us,
                                    double value, int64_t pid) {
    std::string key(metric_name);
    TrackUUID uuid;

    auto it = counter_tracks_.find(key);
    if (it != counter_tracks_.end()) {
        uuid = it->second;
    } else {
        uuid = make_track_uuid(next_counter_uuid_raw_++);
        counter_tracks_[key] = uuid;

        ensure_process_group(pid);
        TrackUUID proc_uuid = make_track_uuid(static_cast<uint64_t>(pid));

        ProtoEncoder packet;
        packet.write_nested(TracePacketFields::track_descriptor, [&](ProtoEncoder &td) {
            td.write_uint64(TrackDescriptorFields::uuid, raw(uuid));
            td.write_uint64(TrackDescriptorFields::parent_uuid, raw(proc_uuid));
            td.write_string(TrackDescriptorFields::name, metric_name);
            td.write_nested(CounterDescriptorFields::counter, [&](ProtoEncoder &cd) {
                (void)cd;
            });
        });
        write_packet(packet.data());
    }

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
        te.write_uint64(TrackEventFields::track_uuid, raw(uuid));
        te.write_tag(kTrackEventDoubleCounterValue, 1);
        uint64_t bits;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        for (int i = 0; i < 8; i++) {
            te.buf_push(static_cast<char>(bits & 0xff));
            bits >>= 8;
        }
    });
    write_packet(packet.data());
    count_++;
}
