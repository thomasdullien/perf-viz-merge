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

// --- Track management ---

uint64_t PerfettoWriter::ensure_thread_track(int64_t pid, int64_t tid) {
    uint64_t uuid = THREAD_UUID_BASE + static_cast<uint64_t>(tid);
    if (!defined_tracks_.count(uuid)) {
        defined_tracks_.insert(uuid);
        emit_thread_track(pid, tid, "");
    }
    return uuid;
}

static const char *child_track_name(ChildTrack type) {
    switch (type) {
    case ChildTrack::SCHED: return "sched";
    case ChildTrack::GIL:   return "GIL";
    case ChildTrack::GPU:   return "GPU";
    }
    return "unknown";
}

static int32_t child_track_sort_rank(ChildTrack type) {
    // Lower rank = appears higher in the UI.
    // Negative values appear above the thread's own call stacks.
    switch (type) {
    case ChildTrack::SCHED: return -3;
    case ChildTrack::GIL:   return -2;
    case ChildTrack::GPU:   return -1;
    }
    return 0;
}

static uint64_t child_track_uuid_base(ChildTrack type) {
    switch (type) {
    case ChildTrack::SCHED: return 200000000ULL;
    case ChildTrack::GIL:   return 300000000ULL;
    case ChildTrack::GPU:   return 400000000ULL;
    }
    return 200000000ULL;
}

uint64_t PerfettoWriter::ensure_child_track(ChildTrack type, int64_t pid,
                                             int64_t tid) {
    uint64_t uuid = child_track_uuid_base(type) + static_cast<uint64_t>(tid);
    if (!defined_tracks_.count(uuid)) {
        defined_tracks_.insert(uuid);
        // Ensure parent thread track exists
        uint64_t parent_uuid = ensure_thread_track(pid, tid);
        emit_child_track(uuid, parent_uuid,
                         child_track_name(type),
                         child_track_sort_rank(type),
                         pid, tid);
    }
    return uuid;
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
    uint64_t uuid = THREAD_UUID_BASE + static_cast<uint64_t>(tid);

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
        // Enable explicit child ordering so sched/GIL/GPU are ordered
        td.write_uint64(TrackDescriptorFields::child_ordering, CHILD_ORDERING_EXPLICIT);
    });
    write_packet(packet.data());
}

void PerfettoWriter::emit_child_track(uint64_t uuid, uint64_t parent_uuid,
                                       std::string_view name,
                                       int32_t sort_rank,
                                       int64_t pid, int64_t tid) {
    // Perfetto merges tracks with the same (pid,tid) in ThreadDescriptor,
    // so child tracks need a unique synthetic TID to stay separate.
    // The UUID already encodes the type (200M/300M/400M + tid), so we
    // use it directly as the fake TID. The name field provides the
    // human-readable label in the UI.
    int64_t fake_tid = static_cast<int64_t>(uuid);

    ProtoEncoder packet;
    packet.write_nested(TracePacketFields::track_descriptor, [&](ProtoEncoder &td) {
        td.write_uint64(TrackDescriptorFields::uuid, uuid);
        td.write_string(TrackDescriptorFields::name, name);
        td.write_int64(TrackDescriptorFields::sibling_order_rank, sort_rank);
        td.write_nested(TrackDescriptorFields::thread, [&](ProtoEncoder &thd) {
            thd.write_int64(ThreadDescriptorFields::pid, pid);
            thd.write_int64(ThreadDescriptorFields::tid, fake_tid);
            thd.write_string(ThreadDescriptorFields::thread_name, name);
        });
    });
    write_packet(packet.data());
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

// Internal: emit a track event on an already-resolved UUID
void PerfettoWriter::write_track_event(std::string_view name,
                                        std::string_view cat, double ts_us,
                                        int64_t pid, int64_t tid,
                                        uint64_t event_type) {
    uint64_t uuid = ensure_thread_track(pid, tid);
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
        te.write_uint64(TrackEventFields::track_uuid, uuid);
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

// Internal: emit a track event on a child track UUID
void PerfettoWriter::write_child_track_event(
        ChildTrack type, std::string_view name, std::string_view cat,
        double ts_us, int64_t pid, int64_t tid, uint64_t event_type) {
    uint64_t uuid = ensure_child_track(type, pid, tid);
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
        te.write_uint64(TrackEventFields::track_uuid, uuid);
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

// --- Public thread track methods ---

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

// --- Public child track methods ---

void PerfettoWriter::write_child_complete(ChildTrack type, std::string_view name,
                                           std::string_view cat, double ts_us,
                                           double dur_us, int64_t pid, int64_t tid) {
    write_child_track_event(type, name, cat, ts_us, pid, tid, TrackEventType::SLICE_BEGIN);
    count_--;
    write_child_track_event(type, name, cat, ts_us + dur_us, pid, tid, TrackEventType::SLICE_END);
}

void PerfettoWriter::write_child_begin(ChildTrack type, std::string_view name,
                                        std::string_view cat, double ts_us,
                                        int64_t pid, int64_t tid) {
    write_child_track_event(type, name, cat, ts_us, pid, tid, TrackEventType::SLICE_BEGIN);
}

void PerfettoWriter::write_child_end(ChildTrack type, std::string_view name,
                                      std::string_view cat, double ts_us,
                                      int64_t pid, int64_t tid) {
    write_child_track_event(type, name, cat, ts_us, pid, tid, TrackEventType::SLICE_END);
}

// --- Other event types ---

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

void PerfettoWriter::write_metadata(std::string_view name_key, int64_t pid,
                                     int64_t tid, std::string_view args_json) {
    std::string name = parse_name_from_args(args_json);

    if (name_key == "process_name") {
        emit_process_track(pid, name);
    } else if (name_key == "thread_name") {
        uint64_t uuid = THREAD_UUID_BASE + static_cast<uint64_t>(tid);
        if (!defined_tracks_.count(uuid)) {
            emit_thread_track(pid, tid, name);
            defined_tracks_.insert(uuid);
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

// --- Counter tracks ---

void PerfettoWriter::write_counter(std::string_view metric_name, double ts_us,
                                    double value, int64_t pid) {
    std::string key(metric_name);
    uint64_t uuid;

    auto it = counter_tracks_.find(key);
    if (it != counter_tracks_.end()) {
        uuid = it->second;
    } else {
        uuid = next_counter_uuid_++;
        counter_tracks_[key] = uuid;

        uint64_t proc_uuid = static_cast<uint64_t>(pid);
        if (!defined_tracks_.count(proc_uuid)) {
            emit_process_track(pid, "");
        }

        ProtoEncoder packet;
        packet.write_nested(TracePacketFields::track_descriptor, [&](ProtoEncoder &td) {
            td.write_uint64(TrackDescriptorFields::uuid, uuid);
            td.write_uint64(TrackDescriptorFields::parent_uuid, proc_uuid);
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
        te.write_uint64(TrackEventFields::track_uuid, uuid);
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
