#pragma once

#include <cstdint>

// Structures matching the perf.data binary format.
// Based on linux/tools/perf/Documentation/perf.data-file-format.txt
// and include/uapi/linux/perf_event.h

namespace perf_format {

constexpr uint64_t MAGIC = 0x32454C4946524550ULL; // "PERFILE2"
constexpr uint64_t MAGIC_SWAPPED = 0x50455246494C4532ULL;

struct FileSection {
    uint64_t offset;
    uint64_t size;
};

struct FileHeader {
    uint64_t magic;
    uint64_t size;        // size of this header struct
    uint64_t attr_size;   // size of each perf_event_attr in attrs section
    FileSection attrs;
    FileSection data;
    FileSection event_types;
    uint64_t flags[4];    // feature flags bitmap (256 bits)
};

static_assert(sizeof(FileHeader) == 104, "FileHeader must be 104 bytes");

// Feature bit indices (in the flags bitmap)
enum Feature : int {
    FEAT_TRACING_DATA = 1,
    FEAT_BUILD_ID = 2,
    FEAT_HOSTNAME = 3,
    FEAT_OSRELEASE = 4,
    FEAT_VERSION = 5,
    FEAT_ARCH = 6,
    FEAT_NRCPUS = 7,
    FEAT_CPUDESC = 8,
    FEAT_CPUID = 9,
    FEAT_TOTAL_MEM = 10,
    FEAT_CMDLINE = 11,
    FEAT_EVENT_DESC = 12,
    FEAT_CPU_TOPOLOGY = 13,
    FEAT_NUMA_TOPOLOGY = 14,
    FEAT_BRANCH_STACK = 15,
    FEAT_PMU_MAPPINGS = 16,
    FEAT_GROUP_DESC = 17,
    FEAT_AUXTRACE = 18,
    FEAT_STAT = 19,
    FEAT_CACHE = 20,
    FEAT_SAMPLE_TIME = 21,
    FEAT_SAMPLE_TOPOLOGY = 22,
    FEAT_CLOCK_DATA = 23,
};

// Subset of perf_event_attr that we need.
// The full struct is variable-size (attr_size field in header tells us).
// We read only what we need and skip the rest.
struct EventAttr {
    uint32_t type;
    uint32_t size;
    uint64_t config;
    union {
        uint64_t sample_period;
        uint64_t sample_freq;
    };
    uint64_t sample_type;
    uint64_t read_format;
    uint64_t flags;  // bit field with disabled, inherit, pinned, exclusive, etc.
    uint32_t wakeup_events_or_watermark;
    uint32_t bp_type;
    union {
        uint64_t bp_addr;
        uint64_t kprobe_func;
        uint64_t uprobe_path;
        uint64_t config1;
    };
    union {
        uint64_t bp_len;
        uint64_t kprobe_addr;
        uint64_t probe_offset;
        uint64_t config2;
    };
    uint64_t branch_sample_type;
    uint64_t sample_regs_user;
    uint32_t sample_stack_user;
    int32_t  clockid;
    uint64_t sample_regs_intr;
    uint64_t aux_watermark;
    uint16_t sample_max_stack;
    uint16_t __reserved_2;
    uint32_t aux_sample_size;
    uint32_t __reserved_3;
    uint64_t sig_data;
    // May be extended in newer kernels; we read up to attr_size bytes
};

// perf_event_header - precedes every record in the data section
struct EventHeader {
    uint32_t type;
    uint16_t misc;
    uint16_t size;
};

// Record types (from perf_event.h)
enum RecordType : uint32_t {
    PERF_RECORD_MMAP = 1,
    PERF_RECORD_LOST = 2,
    PERF_RECORD_COMM = 3,
    PERF_RECORD_EXIT = 4,
    PERF_RECORD_THROTTLE = 5,
    PERF_RECORD_UNTHROTTLE = 6,
    PERF_RECORD_FORK = 7,
    PERF_RECORD_READ = 8,
    PERF_RECORD_SAMPLE = 9,
    PERF_RECORD_MMAP2 = 10,
    PERF_RECORD_AUX = 11,
    PERF_RECORD_ITRACE_START = 12,
    PERF_RECORD_LOST_SAMPLES = 13,
    PERF_RECORD_SWITCH = 14,
    PERF_RECORD_SWITCH_CPU_WIDE = 15,
    PERF_RECORD_NAMESPACES = 16,
    PERF_RECORD_KSYMBOL = 17,
    PERF_RECORD_BPF_EVENT = 18,
    PERF_RECORD_CGROUP = 19,
    PERF_RECORD_TEXT_POKE = 20,
    PERF_RECORD_AUX_OUTPUT_HW_ID = 21,

    // Synthesized by perf tool (type >= 64)
    PERF_RECORD_HEADER_ATTR = 64,
    PERF_RECORD_HEADER_EVENT_TYPE = 65,
    PERF_RECORD_HEADER_TRACING_DATA = 66,
    PERF_RECORD_HEADER_BUILD_ID = 67,
    PERF_RECORD_FINISHED_ROUND = 68,
    PERF_RECORD_ID_INDEX = 69,
    PERF_RECORD_AUXTRACE_INFO = 70,
    PERF_RECORD_AUXTRACE = 71,
    PERF_RECORD_AUXTRACE_ERROR = 72,
    PERF_RECORD_THREAD_MAP = 73,
    PERF_RECORD_CPU_MAP = 74,
    PERF_RECORD_STAT_CONFIG = 75,
    PERF_RECORD_STAT = 76,
    PERF_RECORD_STAT_ROUND = 77,
    PERF_RECORD_EVENT_UPDATE = 78,
    PERF_RECORD_TIME_CONV = 79,
    PERF_RECORD_HEADER_FEATURE = 80,
    PERF_RECORD_COMPRESSED = 81,
    PERF_RECORD_FINISHED_INIT = 82,
};

// Sample type flags (which fields are present in PERF_RECORD_SAMPLE)
enum SampleType : uint64_t {
    PERF_SAMPLE_IP = 1ULL << 0,
    PERF_SAMPLE_TID = 1ULL << 1,
    PERF_SAMPLE_TIME = 1ULL << 2,
    PERF_SAMPLE_ADDR = 1ULL << 3,
    PERF_SAMPLE_READ = 1ULL << 4,
    PERF_SAMPLE_CALLCHAIN = 1ULL << 5,
    PERF_SAMPLE_ID = 1ULL << 6,
    PERF_SAMPLE_CPU = 1ULL << 7,
    PERF_SAMPLE_PERIOD = 1ULL << 8,
    PERF_SAMPLE_STREAM_ID = 1ULL << 9,
    PERF_SAMPLE_RAW = 1ULL << 10,
    PERF_SAMPLE_BRANCH_STACK = 1ULL << 11,
    PERF_SAMPLE_REGS_USER = 1ULL << 12,
    PERF_SAMPLE_STACK_USER = 1ULL << 13,
    PERF_SAMPLE_WEIGHT = 1ULL << 14,
    PERF_SAMPLE_DATA_SRC = 1ULL << 15,
    PERF_SAMPLE_IDENTIFIER = 1ULL << 16,
    PERF_SAMPLE_TRANSACTION = 1ULL << 17,
    PERF_SAMPLE_REGS_INTR = 1ULL << 18,
    PERF_SAMPLE_PHYS_ADDR = 1ULL << 19,
    PERF_SAMPLE_AUX = 1ULL << 20,
    PERF_SAMPLE_CGROUP = 1ULL << 21,
    PERF_SAMPLE_DATA_PAGE_SIZE = 1ULL << 22,
    PERF_SAMPLE_CODE_PAGE_SIZE = 1ULL << 23,
    PERF_SAMPLE_WEIGHT_STRUCT = 1ULL << 24,
};

// perf_event_attr type field values
enum AttrType : uint32_t {
    PERF_TYPE_HARDWARE = 0,
    PERF_TYPE_SOFTWARE = 1,
    PERF_TYPE_TRACEPOINT = 2,
    PERF_TYPE_HW_CACHE = 3,
    PERF_TYPE_RAW = 4,
    PERF_TYPE_BREAKPOINT = 5,
};

// Misc field flags
enum MiscFlags : uint16_t {
    PERF_RECORD_MISC_CPUMODE_MASK = 0x7,
    PERF_RECORD_MISC_KERNEL = 1,
    PERF_RECORD_MISC_USER = 2,
    PERF_RECORD_MISC_HYPERVISOR = 3,
    PERF_RECORD_MISC_GUEST_KERNEL = 4,
    PERF_RECORD_MISC_GUEST_USER = 5,
    PERF_RECORD_MISC_COMM_EXEC = 1 << 13,
};

// attr.flags bit for sample_id_all
constexpr uint64_t ATTR_FLAG_SAMPLE_ID_ALL = 1ULL << 18;

} // namespace perf_format
