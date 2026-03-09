#include "perf_data_reader.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <traceevent/event-parse.h>

#include <fmt/format.h>

using namespace perf_format;

// Helper to read a value from a buffer and advance the pointer
template <typename T>
static T read_val(const uint8_t *&p) {
    T val;
    std::memcpy(&val, p, sizeof(T));
    p += sizeof(T);
    return val;
}

PerfDataReader::PerfDataReader(const std::string &path) {
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        throw std::runtime_error(
            fmt::format("Cannot open {}: {}", path, strerror(errno)));
    }

    struct stat st;
    if (fstat(fd_, &st) < 0) {
        close(fd_);
        throw std::runtime_error(
            fmt::format("Cannot stat {}: {}", path, strerror(errno)));
    }
    mmap_size_ = static_cast<size_t>(st.st_size);

    mmap_base_ = static_cast<uint8_t *>(
        mmap(nullptr, mmap_size_, PROT_READ, MAP_PRIVATE, fd_, 0));
    if (mmap_base_ == MAP_FAILED) {
        mmap_base_ = nullptr;
        close(fd_);
        throw std::runtime_error(
            fmt::format("Cannot mmap {}: {}", path, strerror(errno)));
    }

    tep_ = tep_alloc();
    if (!tep_) {
        throw std::runtime_error("Failed to allocate tep_handle");
    }

    parse_header();
    parse_attrs();
    parse_feature_sections();
}

PerfDataReader::~PerfDataReader() {
    if (tep_) {
        tep_free(tep_);
    }
    if (mmap_base_) {
        munmap(mmap_base_, mmap_size_);
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

void PerfDataReader::parse_header() {
    if (mmap_size_ < sizeof(FileHeader)) {
        throw std::runtime_error("File too small for perf.data header");
    }

    std::memcpy(&header_, mmap_base_, sizeof(FileHeader));

    if (header_.magic != MAGIC) {
        if (header_.magic == MAGIC_SWAPPED) {
            throw std::runtime_error(
                "perf.data file has swapped endianness (cross-arch); not supported");
        }
        throw std::runtime_error(
            fmt::format("Not a perf.data file (bad magic: {:#x})", header_.magic));
    }
}

void PerfDataReader::parse_attrs() {
    if (header_.attrs.size == 0 || header_.attr_size == 0) {
        return; // No attrs
    }

    size_t num_attrs = header_.attrs.size / header_.attr_size;
    const uint8_t *attr_base = mmap_base_ + header_.attrs.offset;

    // Each entry in the attrs section consists of:
    //   perf_event_attr (attr_size bytes) followed by a FileSection
    //   describing the IDs for this attr.
    // Wait -- actually, the layout depends on the file version.
    // In standard perf.data files, the attrs section contains pairs of:
    //   { perf_event_attr, FileSection ids }
    // So each entry is attr_size + sizeof(FileSection) = attr_size + 16 bytes.

    // However, if the attrs.size / attr_size gives us the right count
    // without the FileSection, it may be the simpler format.
    // Let's try the paired format first (attr_size + 16 per entry).

    size_t entry_size_with_ids = header_.attr_size + sizeof(FileSection);
    size_t num_attrs_with_ids = header_.attrs.size / entry_size_with_ids;

    if (num_attrs_with_ids > 0 &&
        num_attrs_with_ids * entry_size_with_ids == header_.attrs.size) {
        // Paired format: attr + FileSection
        num_attrs = num_attrs_with_ids;
        for (size_t i = 0; i < num_attrs; i++) {
            const uint8_t *entry = attr_base + i * entry_size_with_ids;

            EventAttr attr{};
            size_t copy_size = std::min(header_.attr_size, (uint64_t)sizeof(EventAttr));
            std::memcpy(&attr, entry, copy_size);
            attrs_.push_back(attr);

            // Read the IDs FileSection
            FileSection ids_section;
            std::memcpy(&ids_section, entry + header_.attr_size, sizeof(FileSection));

            std::vector<uint64_t> ids;
            if (ids_section.size > 0) {
                size_t num_ids = ids_section.size / sizeof(uint64_t);
                const uint64_t *id_ptr =
                    reinterpret_cast<const uint64_t *>(mmap_base_ + ids_section.offset);
                for (size_t j = 0; j < num_ids; j++) {
                    ids.push_back(id_ptr[j]);
                    id_to_attr_[id_ptr[j]] = i;
                }
            }
            attr_ids_.push_back(std::move(ids));
            attr_names_.push_back(""); // Will be filled by EVENT_DESC feature
        }
    } else {
        // Simple format: just attrs, no IDs section
        num_attrs = header_.attrs.size / header_.attr_size;
        for (size_t i = 0; i < num_attrs; i++) {
            const uint8_t *entry = attr_base + i * header_.attr_size;

            EventAttr attr{};
            size_t copy_size = std::min(header_.attr_size, (uint64_t)sizeof(EventAttr));
            std::memcpy(&attr, entry, copy_size);
            attrs_.push_back(attr);
            attr_ids_.push_back({});
            attr_names_.push_back("");
        }
    }
}

void PerfDataReader::parse_feature_sections() {
    // Feature sections are stored after the data section.
    // The flags bitmap in the header tells us which features are present.
    // For each set bit, there's a FileSection at a known offset after the data.

    // Count features present
    std::vector<int> present_features;
    for (int bit = 0; bit < 256; bit++) {
        int word = bit / 64;
        int pos = bit % 64;
        if (header_.flags[word] & (1ULL << pos)) {
            present_features.push_back(bit);
        }
    }

    if (present_features.empty()) {
        return;
    }

    // Feature sections are stored as an array of FileSection structs
    // starting right after the data section.
    uint64_t feature_offset = header_.data.offset + header_.data.size;

    for (size_t i = 0; i < present_features.size(); i++) {
        uint64_t sec_offset = feature_offset + i * sizeof(FileSection);
        if (sec_offset + sizeof(FileSection) > mmap_size_) {
            break;
        }

        FileSection section;
        std::memcpy(&section, mmap_base_ + sec_offset, sizeof(FileSection));

        if (section.offset + section.size > mmap_size_) {
            continue; // Skip invalid sections
        }

        int feature = present_features[i];
        switch (feature) {
        case FEAT_EVENT_DESC:
            parse_event_desc_section(mmap_base_ + section.offset, section.size);
            break;
        // Could handle FEAT_TRACING_DATA here for libtraceevent format strings
        default:
            break;
        }
    }
}

void PerfDataReader::parse_event_desc_section(const uint8_t *data, size_t size) {
    // EVENT_DESC section format:
    //   u32 nr_events
    //   u32 attr_size
    //   for each event:
    //     perf_event_attr (attr_size bytes)
    //     u32 nr_ids
    //     string event_name (null-terminated, padded to 4-byte boundary)
    //     u64 ids[nr_ids]

    if (size < 8) return;

    const uint8_t *p = data;
    const uint8_t *end = data + size;

    uint32_t nr_events = read_val<uint32_t>(p);
    uint32_t attr_size = read_val<uint32_t>(p);

    for (uint32_t i = 0; i < nr_events && p < end; i++) {
        // Skip the attr
        if (p + attr_size > end) break;
        p += attr_size;

        if (p + 4 > end) break;
        uint32_t nr_ids = read_val<uint32_t>(p);

        // Read event name: u32 str_size followed by str_size bytes (null-padded)
        if (p + 4 > end) break;
        uint32_t str_size = read_val<uint32_t>(p);
        if (p + str_size > end) break;
        const char *name_start = reinterpret_cast<const char *>(p);
        size_t name_len = strnlen(name_start, str_size);
        std::string name(name_start, name_len);
        p += str_size;

        // Read IDs
        if (p + nr_ids * 8 > end) break;
        for (uint32_t j = 0; j < nr_ids; j++) {
            uint64_t id = read_val<uint64_t>(p);
            if (i < attrs_.size()) {
                id_to_attr_[id] = i;
            }
        }

        // Store the name
        if (i < attr_names_.size()) {
            attr_names_[i] = name;
        }
    }
}

size_t PerfDataReader::find_attr_index(const uint8_t *record, size_t record_size) {
    if (attrs_.size() <= 1) {
        return 0;
    }
    if (attrs_.empty()) return 0;

    // For PERF_RECORD_SAMPLE, extract the ID by parsing fields in the order
    // defined by the perf ABI (perf_event_open(2) man page):
    //   IDENTIFIER, IP, TID, TIME, ADDR, ID, STREAM_ID, CPU, PERIOD,
    //   READ, CALLCHAIN, BRANCH_STACK, ...
    // Note: this is NOT strict bit order!
    const auto &attr = attrs_[0];
    const uint8_t *p = record + sizeof(EventHeader);
    const uint8_t *end = record + record_size;
    uint64_t st = attr.sample_type;

    if (st & PERF_SAMPLE_IDENTIFIER) {
        if (p + 8 > end) return 0;
        uint64_t id;
        std::memcpy(&id, p, 8);
        auto it = id_to_attr_.find(id);
        if (it != id_to_attr_.end()) return it->second;
        p += 8;
    }
    if (st & PERF_SAMPLE_IP)   { if (p + 8 > end) return 0; p += 8; }
    if (st & PERF_SAMPLE_TID)  { if (p + 8 > end) return 0; p += 8; }
    if (st & PERF_SAMPLE_TIME) { if (p + 8 > end) return 0; p += 8; }
    if (st & PERF_SAMPLE_ADDR) { if (p + 8 > end) return 0; p += 8; }
    if (st & PERF_SAMPLE_ID) {
        if (p + 8 > end) return 0;
        uint64_t id;
        std::memcpy(&id, p, 8);
        auto it = id_to_attr_.find(id);
        if (it != id_to_attr_.end()) return it->second;
    }

    return 0;
}

PerfEventType PerfDataReader::classify_event(size_t attr_index) const {
    if (attr_index >= attr_names_.size()) {
        return PerfEventType::Other;
    }

    const std::string &name = attr_names_[attr_index];

    if (name.find("sched_switch") != std::string::npos ||
        name.find("sched:sched_switch") != std::string::npos) {
        return PerfEventType::SchedSwitch;
    }
    if (name.find("sched_wakeup") != std::string::npos ||
        name.find("sched:sched_wakeup") != std::string::npos) {
        return PerfEventType::SchedWakeup;
    }
    if (name.find("sched_process_fork") != std::string::npos ||
        name.find("sched:sched_process_fork") != std::string::npos) {
        return PerfEventType::SchedFork;
    }
    if (name.find("sched_stat_runtime") != std::string::npos) {
        return PerfEventType::SchedStatRuntime;
    }
    if (name.find("context-switches") != std::string::npos ||
        name.find("context_switches") != std::string::npos) {
        return PerfEventType::ContextSwitch;
    }
    // GIL probes — check more specific names first
    // perf may name return probes as "take_gil_return" or "take_gil_return__return"
    if (name.find("take_gil_return") != std::string::npos ||
        name.find("take_gil_ret") != std::string::npos ||
        name.find("take_gil__return") != std::string::npos) {
        return PerfEventType::TakeGilReturn;
    }
    if (name.find("take_gil") != std::string::npos) {
        return PerfEventType::TakeGil;
    }
    if (name.find("drop_gil") != std::string::npos) {
        return PerfEventType::DropGil;
    }
    // NVIDIA CUDA probes — order matters: check more specific names first
    // Kernel launch
    if (name.find("nvidia:launch_ret") != std::string::npos) {
        return PerfEventType::NvidiaLaunchReturn;
    }
    if (name.find("nvidia:launch") != std::string::npos) {
        return PerfEventType::NvidiaLaunch;
    }
    // Stream synchronize
    if (name.find("nvidia:stream_sync_ret") != std::string::npos) {
        return PerfEventType::NvidiaStreamSyncReturn;
    }
    if (name.find("nvidia:stream_sync") != std::string::npos) {
        return PerfEventType::NvidiaStreamSync;
    }
    // Device synchronize
    if (name.find("nvidia:dev_sync_ret") != std::string::npos) {
        return PerfEventType::NvidiaDeviceSyncReturn;
    }
    if (name.find("nvidia:dev_sync") != std::string::npos) {
        return PerfEventType::NvidiaDeviceSync;
    }
    // Event synchronize
    if (name.find("nvidia:event_sync_ret") != std::string::npos) {
        return PerfEventType::NvidiaEventSyncReturn;
    }
    if (name.find("nvidia:event_sync") != std::string::npos) {
        return PerfEventType::NvidiaEventSync;
    }
    // Memory transfers — check return variants first
    if (name.find("nvidia:memcpy_htod_ret") != std::string::npos) {
        return PerfEventType::NvidiaMemcpyHtoDReturn;
    }
    if (name.find("nvidia:memcpy_htod") != std::string::npos) {
        return PerfEventType::NvidiaMemcpyHtoD;
    }
    if (name.find("nvidia:memcpy_dtoh_ret") != std::string::npos) {
        return PerfEventType::NvidiaMemcpyDtoHReturn;
    }
    if (name.find("nvidia:memcpy_dtoh") != std::string::npos) {
        return PerfEventType::NvidiaMemcpyDtoH;
    }
    if (name.find("nvidia:memcpy_dtod_ret") != std::string::npos) {
        return PerfEventType::NvidiaMemcpyDtoDReturn;
    }
    if (name.find("nvidia:memcpy_dtod") != std::string::npos) {
        return PerfEventType::NvidiaMemcpyDtoD;
    }
    if (name.find("nvidia:memcpy_async_ret") != std::string::npos) {
        return PerfEventType::NvidiaMemcpyAsyncReturn;
    }
    if (name.find("nvidia:memcpy_async") != std::string::npos) {
        return PerfEventType::NvidiaMemcpyAsync;
    }
    if (name.find("nvidia:memcpy_peer_ret") != std::string::npos) {
        return PerfEventType::NvidiaMemcpyPeerReturn;
    }
    if (name.find("nvidia:memcpy_peer") != std::string::npos) {
        return PerfEventType::NvidiaMemcpyPeer;
    }
    // Memory allocation
    if (name.find("nvidia:malloc_ret") != std::string::npos) {
        return PerfEventType::NvidiaMallocReturn;
    }
    if (name.find("nvidia:malloc") != std::string::npos) {
        return PerfEventType::NvidiaMalloc;
    }
    if (name.find("nvidia:free_ret") != std::string::npos) {
        return PerfEventType::NvidiaFreeReturn;
    }
    if (name.find("nvidia:free") != std::string::npos) {
        return PerfEventType::NvidiaFree;
    }
    // NCCL probes
    if (name.find("nccl:allreduce_ret") != std::string::npos) {
        return PerfEventType::NcclAllReduceReturn;
    }
    if (name.find("nccl:allreduce") != std::string::npos) {
        return PerfEventType::NcclAllReduce;
    }
    if (name.find("nccl:broadcast_ret") != std::string::npos) {
        return PerfEventType::NcclBroadcastReturn;
    }
    if (name.find("nccl:broadcast") != std::string::npos) {
        return PerfEventType::NcclBroadcast;
    }
    if (name.find("nccl:reducescatter_ret") != std::string::npos) {
        return PerfEventType::NcclReduceScatterReturn;
    }
    if (name.find("nccl:reducescatter") != std::string::npos) {
        return PerfEventType::NcclReduceScatter;
    }

    // Fallback: identify software events by attr type/config
    if (attr_index < attrs_.size()) {
        const auto &attr = attrs_[attr_index];
        if (attr.type == PERF_TYPE_SOFTWARE && attr.config == 3) {
            return PerfEventType::ContextSwitch;  // PERF_COUNT_SW_CONTEXT_SWITCHES
        }
    }

    return PerfEventType::Other;
}

bool PerfDataReader::decode_sample(const uint8_t *payload, size_t payload_size,
                                   const EventAttr &attr, PerfEvent &out) {
    const uint8_t *p = payload;
    const uint8_t *end = payload + payload_size;

    uint64_t st = attr.sample_type;

    // Fields are in the order defined by the perf ABI (perf_event_open(2)):
    //   IDENTIFIER, IP, TID, TIME, ADDR, ID, STREAM_ID, CPU, PERIOD,
    //   READ, CALLCHAIN, BRANCH_STACK, REGS_USER, STACK_USER,
    //   WEIGHT/WEIGHT_STRUCT, DATA_SRC, TRANSACTION, REGS_INTR,
    //   PHYS_ADDR, CGROUP, DATA_PAGE_SIZE, CODE_PAGE_SIZE, AUX, RAW
    // Note: this is NOT bit order!

    if (st & PERF_SAMPLE_IDENTIFIER) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_IP) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_TID) {
        if (p + 8 > end) return false;
        out.pid = read_val<int32_t>(p);
        out.tid = read_val<int32_t>(p);
    }
    if (st & PERF_SAMPLE_TIME) {
        if (p + 8 > end) return false;
        out.timestamp_ns = read_val<uint64_t>(p);
    }
    if (st & PERF_SAMPLE_ADDR) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_ID) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_STREAM_ID) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_CPU) {
        if (p + 8 > end) return false;
        out.cpu = read_val<int32_t>(p);
        p += 4; // skip reserved
    }
    if (st & PERF_SAMPLE_PERIOD) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_READ) {
        return false; // variable-size, too complex
    }
    if (st & PERF_SAMPLE_CALLCHAIN) {
        if (p + 8 > end) return false;
        uint64_t nr = read_val<uint64_t>(p);
        if (nr > 1024 || p + nr * 8 > end) return false; // sanity check
        p += nr * 8;
    }
    if (st & PERF_SAMPLE_BRANCH_STACK) {
        if (p + 8 > end) return false;
        uint64_t nr = read_val<uint64_t>(p);
        // Each branch entry is 24 bytes (from, to, flags)
        if (p + nr * 24 > end) return false;
        p += nr * 24;
    }
    if (st & PERF_SAMPLE_REGS_USER) {
        // Skip: u64 abi + regs based on sample_regs_user weight
        // Too complex to parse generically; bail
        return false;
    }
    if (st & PERF_SAMPLE_STACK_USER) {
        if (p + 8 > end) return false;
        uint64_t size = read_val<uint64_t>(p);
        if (p + size > end) return false;
        p += size;
        if (size > 0) {
            if (p + 8 > end) return false;
            p += 8; // dyn_size
        }
    }
    // WEIGHT (bit 14) or WEIGHT_STRUCT (bit 24) — both 8 bytes
    if (st & (PERF_SAMPLE_WEIGHT | PERF_SAMPLE_WEIGHT_STRUCT)) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_DATA_SRC) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_TRANSACTION) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_REGS_INTR) {
        return false; // variable, skip
    }
    if (st & PERF_SAMPLE_PHYS_ADDR) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_AUX) {
        if (p + 8 > end) return false;
        uint64_t size = read_val<uint64_t>(p);
        if (p + size > end) return false;
        p += size;
    }
    if (st & PERF_SAMPLE_CGROUP) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_DATA_PAGE_SIZE) {
        if (p + 8 > end) return false;
        p += 8;
    }
    if (st & PERF_SAMPLE_CODE_PAGE_SIZE) {
        if (p + 8 > end) return false;
        p += 8;
    }
    // RAW comes last
    if (st & PERF_SAMPLE_RAW) {
        if (p + 4 > end) return false;
        uint32_t raw_size = read_val<uint32_t>(p);
        if (p + raw_size > end) return false;

        // Use the raw data to decode tracepoint-specific fields
        // based on event type
        const uint8_t *raw_data = p;

        if (out.type == PerfEventType::SchedSwitch) {
            // Raw tracepoint data layout:
            //   Common fields (8 bytes): type(2) + flags(1) + preempt_count(1) + pid(4)
            //   sched_switch fields:
            //     prev_comm: char[16]  (offset 8)
            //     prev_pid:  i32       (offset 24)
            //     prev_prio: i32       (offset 28)
            //     prev_state: i64      (offset 32, kernel 6.x; i32 on older)
            //     next_comm: char[16]  (offset 40)
            //     next_pid:  i32       (offset 56)
            //     next_prio: i32       (offset 60)
            //   Total: 64 bytes
            //
            // Some perf versions strip the common header; try both layouts.

            const size_t COMMON_SIZE = 8;
            size_t base = 0;

            // Heuristic: if raw_size >= 64, common fields are likely included.
            // If raw_size >= 56 but < 64, common fields may be stripped.
            if (raw_size >= COMMON_SIZE + 56) {
                base = COMMON_SIZE; // skip common fields
            } else if (raw_size >= 56) {
                base = 0; // no common fields
            } else {
                // Too small, skip
                p += raw_size;
                return true;
            }

            std::memcpy(out.data.sched_switch.prev_comm, raw_data + base,
                        std::min<size_t>(16, raw_size - base));
            out.data.sched_switch.prev_comm[16] = '\0';

            if (base + 32 <= raw_size) {
                std::memcpy(&out.data.sched_switch.prev_pid, raw_data + base + 16, 4);
                std::memcpy(&out.data.sched_switch.prev_state, raw_data + base + 24, 8);
            }
            if (base + 56 <= raw_size) {
                std::memcpy(out.data.sched_switch.next_comm, raw_data + base + 32,
                            std::min<size_t>(16, raw_size - base - 32));
                out.data.sched_switch.next_comm[16] = '\0';
                std::memcpy(&out.data.sched_switch.next_pid, raw_data + base + 48, 4);
            }

            out.data.sched_switch.prev_tid = out.data.sched_switch.prev_pid;
            out.data.sched_switch.next_tid = out.data.sched_switch.next_pid;
        }

        p += raw_size;
    }

    return true;
}

void PerfDataReader::decode_comm(const uint8_t *payload, size_t payload_size) {
    // COMM record: pid(u32) + tid(u32) + comm(null-terminated string)
    if (payload_size < 8) return;

    const uint8_t *p = payload;
    int32_t pid = read_val<int32_t>(p);
    int32_t tid = read_val<int32_t>(p);

    size_t remaining = payload_size - 8;
    // Account for sample_id suffix - the comm string ends at the first null
    const char *comm = reinterpret_cast<const char *>(p);
    size_t comm_len = strnlen(comm, remaining);
    if (comm_len > 0) {
        comm_map_[tid] = std::string(comm, comm_len);
        if (pid == tid) {
            comm_map_[pid] = std::string(comm, comm_len);
        }
    }
}

bool PerfDataReader::decode_fork(const uint8_t *payload, size_t payload_size,
                                 PerfEvent &out) {
    // FORK record: pid(u32) + ppid(u32) + tid(u32) + ptid(u32) + time(u64)
    if (payload_size < 24) return false;

    const uint8_t *p = payload;
    int32_t pid = read_val<int32_t>(p);
    int32_t ppid = read_val<int32_t>(p);
    int32_t tid = read_val<int32_t>(p);
    int32_t ptid = read_val<int32_t>(p);
    uint64_t time = read_val<uint64_t>(p);

    out.type = PerfEventType::SchedFork;
    out.timestamp_ns = time;
    out.pid = ppid;
    out.tid = ptid;
    out.data.fork.parent_tid = ptid;
    out.data.fork.child_tid = tid;
    out.data.fork.child_pid = pid;
    return true;
}

void PerfDataReader::parse_data_section(EventCallback &cb) {
    const uint8_t *data = mmap_base_ + header_.data.offset;
    const uint8_t *end = data + header_.data.size;

    while (data + sizeof(EventHeader) <= end) {
        EventHeader hdr;
        std::memcpy(&hdr, data, sizeof(EventHeader));

        if (hdr.size < sizeof(EventHeader) || data + hdr.size > end) {
            break; // Invalid or truncated record
        }

        const uint8_t *payload = data + sizeof(EventHeader);
        size_t payload_size = hdr.size - sizeof(EventHeader);

        switch (hdr.type) {
        case PERF_RECORD_SAMPLE: {
            size_t attr_idx = find_attr_index(data, hdr.size);
            if (attr_idx < attrs_.size()) {
                PerfEvent event;
                event.type = classify_event(attr_idx);
                if (decode_sample(payload, payload_size, attrs_[attr_idx], event)) {
                    event_count_++;
                    cb(event);
                }
            }
            break;
        }
        case PERF_RECORD_COMM:
            decode_comm(payload, payload_size);
            break;
        case PERF_RECORD_FORK: {
            PerfEvent event;
            if (decode_fork(payload, payload_size, event)) {
                event_count_++;
                cb(event);
            }
            break;
        }
        default:
            break; // Skip other record types
        }

        data += hdr.size;
    }

}

void PerfDataReader::read_all_events(EventCallback cb) {
    parse_data_section(cb);
}
