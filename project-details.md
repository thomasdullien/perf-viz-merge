# project-details.md: Detailed Design & Implementation Plan for perf-viz-merge

## 1. Overview

**perf-viz-merge** is a C++17 command-line tool that merges:
- A **perf.data** binary file (scheduler events, uprobes for GIL tracking, future NVIDIA probes)
- A **VizTracer JSON** file (Python function-level tracing)

into a single **Chrome Trace Format JSON** file viewable in Perfetto.

### Key challenges
- 30M+ events, 1.5–5 GB of data; must stay under 32 GB RAM
- Two independent clock domains requiring alignment
- perf.data must be parsed via binary APIs (not `perf script` text output)
- VizTracer JSON must be streamed, not loaded fully into memory

---

## 2. Architecture

```
┌─────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  perf.data   │────▶│  PerfDataReader  │────▶│                 │
│  (binary)    │     │  (libperf +      │     │                 │
└─────────────┘     │   libtraceevent) │     │   MergeEngine   │──▶  output.json
                     └──────────────────┘     │   (time-sorted  │     (Chrome Trace
┌─────────────┐     ┌──────────────────┐     │    merge +      │      Format)
│  trace.json  │────▶│  VizJsonReader   │────▶│    streaming    │
│  (VizTracer) │     │  (simdjson       │     │    JSON writer) │
└─────────────┘     │   ondemand)      │     └─────────────────┘
                     └──────────────────┘
```

### Components

| Component | Responsibility |
|-----------|---------------|
| **PerfDataReader** | Opens perf.data via the perf session API (built from kernel source), iterates events, uses libtraceevent to decode tracepoint fields, uses libelf/libdw for uprobe symbol resolution |
| **VizJsonReader** | Streams the VizTracer JSON using simdjson's ondemand API, yields one trace event at a time |
| **ClockAligner** | Detects and applies the T0 offset between perf (CLOCK_MONOTONIC) and VizTracer (epoch-based or 0-based) timestamps |
| **MergeEngine** | Merge-sorts the two time-ordered event streams, converts each event to Chrome Trace JSON, and writes output incrementally |
| **TraceWriter** | Writes Chrome Trace Format JSON using fmt::format, streaming to disk |

---

## 3. Parsing perf.data — Binary API Approach

### 3.1 Why not `perf script`?

The text output of `perf script` changes between kernel versions, has inconsistent field formatting, and is fragile to parse at scale. The project requires direct binary parsing.

### 3.2 Library strategy: Build libperf from kernel source

The system does **not** have a `libperf-dev` package. The `perf` tool statically links its own copy. We will:

1. Download the kernel source matching the target perf version (or use linux-source-6.1)
2. Build `libperf.a` from `tools/lib/perf/` in the kernel tree
3. Install `libtraceevent-dev` from Debian packages (available as `1.7.1-1`)
4. Use `libelf-dev` and `libdw` (from elfutils, already installed) for symbol resolution

### 3.3 Key perf APIs

The libperf API (from `tools/lib/perf/`) provides:

```c
#include <perf/evlist.h>
#include <perf/evsel.h>
#include <perf/mmap.h>
#include <perf/cpumap.h>
#include <perf/threadmap.h>
#include <internal/lib.h>        // perf internal utilities
```

However, the **most practical approach** for reading perf.data files is to use the higher-level **perf session API** from `tools/perf/util/`:

```c
#include "util/session.h"
#include "util/evlist.h"
#include "util/evsel.h"
#include "util/tool.h"
#include "util/event.h"
```

Key structures and flow:

```cpp
// 1. Open a perf.data file
struct perf_session *session;
struct perf_data data = {
    .path = "perf.data",
    .mode = PERF_DATA_MODE_READ,
};

// 2. Define callbacks via perf_tool
struct perf_tool tool = {};
tool.sample = process_sample_event;   // called for each sample
tool.comm   = perf_event__process_comm;
tool.fork   = perf_event__process_fork;
tool.exit   = perf_event__process_exit;

// 3. Create session and process
session = perf_session__new(&data, &tool);
perf_session__process_events(session);  // iterates all events, calls callbacks
perf_session__delete(session);
```

Each sample callback receives:
```cpp
int process_sample_event(struct perf_tool *tool,
                         union perf_event *event,
                         struct perf_sample *sample,
                         struct evsel *evsel,
                         struct machine *machine)
{
    // sample->time   — timestamp (nanoseconds, CLOCK_MONOTONIC)
    // sample->pid    — process ID
    // sample->tid    — thread ID
    // sample->cpu    — CPU number
    // sample->raw_data — raw tracepoint data (for libtraceevent)
    // evsel->name    — event name like "sched:sched_switch"
}
```

### 3.4 Alternative: Standalone perf.data parser (no kernel build dependency)

Building against the full perf source tree is complex and fragile. An alternative — and likely **better** — approach is to write a **standalone perf.data parser** that:

1. Reads the perf.data file header (magic, section offsets, attrs)
2. Iterates the data section which contains `perf_event` records
3. Uses `libtraceevent` to decode tracepoint payloads
4. Uses `libelf`/`libdw` for symbol resolution of uprobe addresses

The perf.data format is documented and relatively stable:

```
┌────────────────────────────────┐
│  perf_file_header              │  magic "PERFILE2", size, attr_offset, data_offset, etc.
├────────────────────────────────┤
│  attrs section                 │  array of perf_event_attr (one per event type)
├────────────────────────────────┤
│  data section                  │  stream of perf_event records
│  ┌──────────────────────────┐  │
│  │ perf_event_header        │  │  type, misc, size
│  │ union { sample, mmap,    │  │
│  │   comm, fork, ... }      │  │
│  └──────────────────────────┘  │
│  ... (millions of records) ... │
├────────────────────────────────┤
│  feature sections (optional)   │  hostname, build-ids, cmdline, etc.
└────────────────────────────────┘
```

**Recommendation**: Use the standalone parser approach. It avoids the kernel build dependency, is self-contained, and we only need to handle a few event types (PERF_RECORD_SAMPLE for tracepoints/uprobes, PERF_RECORD_COMM, PERF_RECORD_FORK, PERF_RECORD_MMAP2).

### 3.5 Decoding tracepoint fields with libtraceevent

```c
#include <traceevent/event-parse.h>

// Initialize
struct tep_handle *tep = tep_alloc();

// Load format strings from tracefs (or from perf.data's HEADER_TRACING_DATA)
// For sched_switch, the format is at:
//   /sys/kernel/tracing/events/sched/sched_switch/format

// Parse a raw tracepoint record
struct tep_record record = {
    .data = sample->raw_data,
    .size = sample->raw_size,
    .ts   = sample->time,
};

struct tep_event *event = tep_find_event_by_record(tep, &record);

// Extract fields
unsigned long long val;
tep_get_field_val(NULL, event, "prev_pid", &record, &val, 0);
int prev_pid = (int)val;

tep_get_field_val(NULL, event, "next_pid", &record, &val, 0);
int next_pid = (int)val;

tep_get_field_val(NULL, event, "prev_state", &record, &val, 0);
long prev_state = (long)val;
```

### 3.6 Uprobe symbol resolution with libelf/libdw

For uprobe events (take_gil, drop_gil, etc.), the event name from perf already contains the probe name (e.g., `python:take_gil`), so symbol resolution may not be needed in most cases. However, for call stack unwinding or future probes:

```c
#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>

// Use libdwfl to resolve addresses to function names
Dwfl *dwfl = dwfl_begin(&callbacks);
dwfl_report_begin(dwfl);
dwfl_linux_proc_report(dwfl, pid);
dwfl_report_end(dwfl, NULL, NULL);

Dwfl_Module *mod = dwfl_addrmodule(dwfl, address);
const char *name = dwfl_module_addrname(mod, address);
```

---

## 4. Parsing VizTracer JSON — Streaming with simdjson

### 4.1 VizTracer output format

VizTracer outputs Chrome Trace Format JSON:
```json
{
  "traceEvents": [
    {"ph": "B", "name": "func_a", "pid": 1234, "tid": 1234, "ts": 1234567.89, ...},
    {"ph": "E", "name": "func_a", "pid": 1234, "tid": 1234, "ts": 1234568.12, ...},
    ...
  ],
  "file_info": { ... },
  "viztracer_metadata": { ... }
}
```

The `traceEvents` array can contain millions of entries. The `ts` field is in **microseconds**.

### 4.2 Streaming with simdjson ondemand

simdjson's ondemand API parses lazily — it doesn't build a full DOM. However, it still requires the full document in memory (or memory-mapped). For files up to ~5 GB, memory-mapping is feasible within the 32 GB limit.

```cpp
#include <simdjson.h>
using namespace simdjson;

// Memory-map the file (simdjson does this automatically)
ondemand::parser parser;
auto doc = parser.iterate(padded_string::load("trace.json"));

// Navigate to traceEvents array
auto events = doc["traceEvents"].get_array();

// Iterate lazily — each event is parsed on access
for (auto event : events) {
    std::string_view ph = event["ph"].get_string();
    std::string_view name = event["name"].get_string();
    double ts = event["ts"].get_double();
    int64_t pid = event["pid"].get_int64();
    int64_t tid = event["tid"].get_int64();

    // Yield this event to the merge engine
    emit_viztrace_event(ph, name, ts, pid, tid);
}
```

**Memory consideration**: simdjson memory-maps the file, so a 5 GB file uses ~5 GB of virtual address space but physical memory usage depends on access patterns. The ondemand API's working set is small (a few MB) since it only materializes values on access.

### 4.3 Handling large files — chunked approach (fallback)

If memory-mapping proves problematic, we can split the JSON file:
1. Scan for the `"traceEvents":[` prefix
2. Read and parse individual JSON objects delimited by commas
3. Use simdjson's `iterate_many()` or manual chunking

This is a fallback; memory-mapping should work for the stated constraints.

---

## 5. Clock Alignment

### 5.1 Clock domains

| Source | Clock | Unit | Epoch |
|--------|-------|------|-------|
| perf.data | CLOCK_MONOTONIC | nanoseconds | system boot |
| VizTracer | `time.monotonic()` (Python) | microseconds | system boot |

When perf is recorded with `-k CLOCK_MONOTONIC`, both sources use the same clock base. The alignment then only requires unit conversion:

```
perf_ts_us = perf_ts_ns / 1000.0
```

### 5.2 Detection and fallback

If VizTracer uses a different clock (e.g., `time.time()` which is epoch-based), we need to compute an offset. Strategy:

1. **Read metadata**: Check VizTracer's `viztracer_metadata` for clock info
2. **Heuristic overlap**: Find the first and last timestamps in both streams; compute offset that aligns the overlapping time range
3. **User-specified offset**: Accept a `--time-offset` flag as manual override

### 5.3 Implementation

```cpp
struct ClockAligner {
    double offset_us = 0.0;  // added to VizTracer timestamps

    // Auto-detect: both use CLOCK_MONOTONIC, just convert units
    void detect(double perf_first_ns, double perf_last_ns,
                double viz_first_us, double viz_last_us) {
        double perf_first_us = perf_first_ns / 1000.0;
        // If timestamps are in the same ballpark, no offset needed
        if (std::abs(perf_first_us - viz_first_us) < 1e6) {
            offset_us = 0.0;  // same clock
        } else {
            // Heuristic: align start times
            offset_us = perf_first_us - viz_first_us;
        }
    }

    double align_viz(double viz_ts_us) const { return viz_ts_us + offset_us; }
    double align_perf(uint64_t perf_ts_ns) const { return perf_ts_ns / 1000.0; }
};
```

---

## 6. Chrome Trace Format Output

### 6.1 Format specification

The output is a JSON file in Chrome Trace Event Format:

```json
{
  "traceEvents": [
    {"ph": "X", "name": "event_name", "cat": "category",
     "ts": 12345.678, "dur": 100.5,
     "pid": 1234, "tid": 5678,
     "args": {"key": "value"}},
    ...
  ],
  "metadata": { "merger": "perf-viz-merge" }
}
```

### 6.2 Event type mapping

| Source Event | Chrome Trace ph | Category | Details |
|---|---|---|---|
| VizTracer B/E/X events | Pass through (B/E/X) | `"python"` | name, args preserved |
| sched:sched_switch (off-cpu) | `"X"` (complete) | `"sched"` | Duration from switch-out to switch-in |
| sched:sched_switch (on-cpu) | `"X"` (complete) | `"sched"` | Duration on CPU |
| sched:sched_wakeup | `"i"` (instant) | `"sched"` | Wakeup event |
| sched:sched_process_fork | `"i"` (instant) | `"sched"` | Fork event |
| python:take_gil | `"B"` (begin) | `"gil"` | GIL acquisition start |
| python:take_gil_return | `"E"` (end) | `"gil"` | GIL acquisition end |
| python:drop_gil | `"i"` (instant) | `"gil"` | GIL release |
| nvidia:launch | `"i"` (instant) | `"gpu"` | Kernel launch |
| nvidia:sync_start | `"B"` (begin) | `"gpu"` | Stream sync start |
| nvidia:sync_end | `"E"` (end) | `"gpu"` | Stream sync end |

### 6.3 Track organization in Perfetto

Each combination of (pid, tid) becomes a track in Perfetto. Additional synthetic tracks:

- **Per-CPU scheduler track**: `pid = CPU_N` (synthetic), shows which thread is on which CPU
- **GIL track**: Per-process track showing GIL contention
- **GPU track**: Per-process track for NVIDIA events

### 6.4 Streaming JSON writer

```cpp
class TraceWriter {
    FILE *out_;
    bool first_ = true;

public:
    TraceWriter(const char *path) : out_(fopen(path, "w")) {
        fmt::print(out_, "{{\"traceEvents\":[\n");
    }

    void write_event(std::string_view ph, std::string_view name,
                     std::string_view cat, double ts_us, double dur_us,
                     int64_t pid, int64_t tid,
                     std::string_view args_json = "{}") {
        if (!first_) fmt::print(out_, ",\n");
        first_ = false;

        if (dur_us > 0) {
            fmt::print(out_,
                R"({{"ph":"X","name":"{}","cat":"{}","ts":{:.3f},"dur":{:.3f},"pid":{},"tid":{},"args":{}}})",
                name, cat, ts_us, dur_us, pid, tid, args_json);
        } else {
            fmt::print(out_,
                R"({{"ph":"{}","name":"{}","cat":"{}","ts":{:.3f},"pid":{},"tid":{},"args":{}}})",
                ph, name, cat, ts_us, pid, tid, args_json);
        }
    }

    ~TraceWriter() {
        fmt::print(out_, "\n]}}\n");
        fclose(out_);
    }
};
```

---

## 7. Merge Engine

### 7.1 Strategy: Two-pass for perf, single-pass merge

**Pass 1 — Perf preprocessing**: Read all perf.data events, build scheduler state machines (to compute sched_switch durations), and store normalized events in a sorted temporary structure.

**Pass 2 — Merge and write**: Merge-iterate the perf events and the VizTracer JSON stream (both sorted by timestamp) and write output.

### 7.2 Scheduler state machine

To produce "on-CPU" and "off-CPU" duration events from point-in-time sched_switch events:

```cpp
struct SchedState {
    // Per-thread: when was this thread last switched in?
    std::unordered_map<int32_t, uint64_t> last_switch_in;  // tid -> timestamp_ns

    // Per-CPU: what thread is currently running?
    std::unordered_map<int32_t, int32_t> cpu_current_tid;  // cpu -> tid
};

// On sched_switch event:
//   prev_tid is switched OUT at timestamp T
//     -> emit "on-cpu" event for prev_tid: duration = T - last_switch_in[prev_tid]
//   next_tid is switched IN at timestamp T
//     -> record last_switch_in[next_tid] = T
```

### 7.3 GIL state machine

```cpp
struct GilState {
    // Per-thread: when did take_gil start?
    std::unordered_map<int32_t, uint64_t> take_gil_start;  // tid -> timestamp_ns

    // On take_gil: record start time
    // On take_gil_return: emit "GIL acquire" duration event
    // On drop_gil: emit instant "GIL release" event
};
```

### 7.4 Memory budget

| Data structure | Estimated size |
|---|---|
| Perf event storage (30M events × ~48 bytes) | ~1.4 GB |
| VizTracer mmap (5 GB file) | ~5 GB virtual (paged) |
| Scheduler state maps | < 100 MB |
| Output buffer | < 100 MB |
| **Total peak** | **~7 GB physical** |

Well within the 32 GB limit.

### 7.5 Perf event internal representation

```cpp
struct PerfEvent {
    uint64_t timestamp_ns;
    int32_t pid;
    int32_t tid;
    int32_t cpu;
    enum class Type : uint8_t {
        SchedSwitch, SchedWakeup, SchedFork,
        TakeGil, TakeGilReturn, DropGil,
        NvidiaLaunch, NvidiaSyncStart, NvidiaSyncEnd,
        Other
    } type;
    // Union of type-specific data
    union {
        struct { int32_t prev_tid; int32_t next_tid; int64_t prev_state; } sched;
        struct { int32_t target_tid; } wakeup;
        struct { int32_t parent_tid; int32_t child_tid; } fork;
    } data;
};
// sizeof ~48 bytes with padding
```

---

## 8. perf.data Binary Parser (Standalone)

### 8.1 File header

```cpp
struct perf_file_header {
    uint64_t magic;          // "PERFILE2" = 0x32454C4946524550
    uint64_t size;           // size of this header
    uint64_t attr_size;      // size of each perf_event_attr
    struct {
        uint64_t offset;
        uint64_t size;
    } attrs, data, event_types;
    uint64_t flags[4];       // feature flags bitmap
};
```

### 8.2 Event attrs

The attrs section contains one `perf_event_attr` per configured event. Each attr tells us:
- `type`: PERF_TYPE_TRACEPOINT, PERF_TYPE_SOFTWARE, etc.
- `config`: tracepoint ID (matches `/sys/kernel/tracing/events/.../id`)
- `sample_type`: bitmask of which fields are in each sample (TID, TIME, CPU, RAW, etc.)
- `sample_id_all`: whether non-sample events also have sample_id fields

### 8.3 Data section parsing

The data section is a stream of records:

```cpp
struct perf_event_header {
    uint32_t type;   // PERF_RECORD_SAMPLE, PERF_RECORD_MMAP2, PERF_RECORD_COMM, etc.
    uint16_t misc;
    uint16_t size;   // total record size including header
};
```

For `PERF_RECORD_SAMPLE` (type=9), the payload fields depend on `sample_type` bits:
- `PERF_SAMPLE_TID` → pid, tid (uint32_t each)
- `PERF_SAMPLE_TIME` → timestamp (uint64_t)
- `PERF_SAMPLE_CPU` → cpu, reserved (uint32_t each)
- `PERF_SAMPLE_RAW` → size (uint32_t) + raw tracepoint data
- `PERF_SAMPLE_CALLCHAIN` → nr + array of ips

The parser must read the `sample_type` from the attr and decode fields in the exact order specified by the perf ABI (bit order = field order).

### 8.4 Tracing data feature section

When perf records tracepoints, it embeds the tracefs format strings in the `HEADER_TRACING_DATA` feature section. This contains:
- Header page format
- Header event format
- Ftrace event formats
- Event system formats (sched/sched_switch format, etc.)

The parser extracts these and feeds them to `libtraceevent` via `tep_parse_event()` so it can decode raw tracepoint data without needing access to the live tracefs filesystem.

### 8.5 Parser class outline

```cpp
class PerfDataReader {
public:
    explicit PerfDataReader(const std::string &path);
    ~PerfDataReader();

    // Iterate all sample events, calling the callback for each
    using EventCallback = std::function<void(const PerfEvent &)>;
    void read_all_events(EventCallback cb);

    // Access metadata
    uint64_t first_timestamp() const;
    uint64_t last_timestamp() const;
    const std::vector<std::string>& event_names() const;

private:
    int fd_;
    void *mmap_base_;
    size_t mmap_size_;
    perf_file_header header_;
    std::vector<perf_event_attr> attrs_;
    std::vector<std::string> attr_names_;

    struct tep_handle *tep_;

    void parse_header();
    void parse_attrs();
    void parse_tracing_data();   // extract format strings for libtraceevent
    void parse_data_section(EventCallback cb);
    PerfEvent decode_sample(const perf_event_header *hdr, const perf_event_attr &attr);
};
```

---

## 9. Build System

### 9.1 Dependencies

| Dependency | Source | Purpose |
|---|---|---|
| simdjson | Header-only or shared lib (`libsimdjson-dev`) | JSON parsing |
| fmt | Header-only or shared lib | Fast string formatting |
| libtraceevent | `libtraceevent-dev` (Debian package) | Tracepoint field decoding |
| libelf | `libelf-dev` (already installed) | ELF parsing for symbol resolution |
| libdw | Part of elfutils (already installed) | DWARF debug info for symbol resolution |

### 9.2 Makefile

```makefile
CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -march=native
LDFLAGS  :=

# Dependencies (from pkg-config or manual)
CXXFLAGS += $(shell pkg-config --cflags libtraceevent 2>/dev/null)
LDFLAGS  += $(shell pkg-config --libs libtraceevent 2>/dev/null)
LDFLAGS  += -lelf -ldw

# simdjson (system or vendored)
CXXFLAGS += $(shell pkg-config --cflags simdjson 2>/dev/null)
LDFLAGS  += $(shell pkg-config --libs simdjson 2>/dev/null || echo "-lsimdjson")

# fmt
LDFLAGS  += -lfmt

SRCS := src/main.cpp src/perf_data_reader.cpp src/viz_json_reader.cpp \
        src/merge_engine.cpp src/trace_writer.cpp src/clock_aligner.cpp
OBJS := $(SRCS:.cpp=.o)
TARGET := perf-viz-merge

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
```

### 9.3 Directory structure

```
perftrace/
├── project.md
├── project-details.md
├── Makefile
├── src/
│   ├── main.cpp              # CLI argument parsing, orchestration
│   ├── perf_data_reader.h    # PerfDataReader class
│   ├── perf_data_reader.cpp  # perf.data binary parser
│   ├── perf_data_format.h    # perf.data structs (header, attr, event types)
│   ├── viz_json_reader.h     # VizJsonReader class
│   ├── viz_json_reader.cpp   # simdjson streaming parser
│   ├── clock_aligner.h       # ClockAligner
│   ├── clock_aligner.cpp
│   ├── merge_engine.h        # MergeEngine
│   ├── merge_engine.cpp
│   ├── trace_writer.h        # TraceWriter (Chrome Trace JSON output)
│   ├── trace_writer.cpp
│   └── event_types.h         # PerfEvent, VizEvent structs
└── test/
    ├── generate_test_data.py # Python script to generate test VizTracer trace
    └── verify.sh             # End-to-end test script
```

---

## 10. Implementation Plan (Step-by-step)

### Phase 1: Scaffolding & Build
1. Create directory structure
2. Set up Makefile with dependency detection
3. Install missing packages (`libtraceevent-dev`, `libsimdjson-dev`, `libfmt-dev`)
4. Write `main.cpp` with CLI arg parsing (input files, output file, optional time offset)
5. Verify compilation

### Phase 2: perf.data Parser
1. Define perf.data format structs in `perf_data_format.h`
2. Implement `PerfDataReader`: header parsing, attr parsing, feature section parsing
3. Implement data section iteration: read `perf_event_header`, dispatch by type
4. Implement `PERF_RECORD_SAMPLE` decoding (field-by-field based on `sample_type`)
5. Integrate libtraceevent: extract tracing data feature, call `tep_parse_event()`, decode sched_switch fields
6. Handle `PERF_RECORD_COMM` and `PERF_RECORD_FORK` for pid/tid→comm mapping
7. Test with real perf.data file

### Phase 3: VizTracer JSON Reader
1. Implement `VizJsonReader` using simdjson ondemand
2. Memory-map the JSON file, navigate to `traceEvents` array
3. Iterate events, extract ph/name/cat/ts/pid/tid/args
4. Test with real VizTracer output

### Phase 4: Clock Alignment
1. Implement `ClockAligner` with auto-detection
2. First pass: scan both sources for timestamp ranges
3. Apply offset during merge

### Phase 5: Merge & Output
1. Implement `TraceWriter` for streaming JSON output
2. Implement scheduler state machine (sched_switch → duration events)
3. Implement GIL state machine (take_gil/take_gil_return → duration events)
4. Implement `MergeEngine`: merge-sort both streams by timestamp, write output
5. Handle per-CPU scheduler tracks with synthetic PIDs

### Phase 6: Testing & Verification
1. Create `test/generate_test_data.py`: small Python program instrumented with VizTracer
2. Create `test/verify.sh`: runs perf record + viztracer, runs perf-viz-merge, validates output
3. Use Perfetto's `trace_processor_shell` to verify the merged file loads correctly
4. Validate event counts

---

## 11. CLI Interface

```
Usage: perf-viz-merge [options] --perf <perf.data> --viz <trace.json> -o <output.json>

Options:
  --perf <path>          Path to perf.data file
  --viz <path>           Path to VizTracer JSON file
  -o, --output <path>    Output Chrome Trace JSON file (default: merged.json)
  --time-offset <us>     Manual time offset in microseconds (added to VizTracer timestamps)
  --filter-pid <pid>     Only include events for this PID (and its threads)
  --no-sched             Omit scheduler events
  --no-gil               Omit GIL tracking events
  -v, --verbose          Print progress information
  -h, --help             Show this help
```

---

## 12. Risk Assessment & Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| perf.data format varies between kernel versions | Medium | Support common versions (5.x, 6.x); validate magic and attr size; fail gracefully on unknown features |
| libtraceevent format strings not embedded in perf.data | Low | Fallback: read from live tracefs `/sys/kernel/tracing/events/` |
| VizTracer uses epoch clock instead of CLOCK_MONOTONIC | Medium | Auto-detection heuristic + manual `--time-offset` flag |
| simdjson memory-mapping 5GB file fails | Low | Fallback to chunked reading |
| Sample_type field ordering assumptions wrong | Medium | Careful implementation following kernel ABI exactly; test with real data early |
| Endianness mismatch (cross-architecture perf.data) | Low | Detect from header; only support native endianness initially |

---

## 13. Testing Strategy

### Unit tests
- Parse a known perf.data file header and verify attr extraction
- Decode a sample record with known sample_type bitmask
- Parse a small VizTracer JSON and verify event extraction
- Clock alignment with known offsets

### Integration test (end-to-end)
```bash
#!/bin/bash
# test/verify.sh

# 1. Generate VizTracer trace
python3 test/generate_test_data.py  # produces trace.json

# 2. Record perf data (requires root/perf_event_paranoid)
sudo perf record -e sched:sched_switch -k CLOCK_MONOTONIC \
    -o perf.data -- python3 test/generate_test_data.py

# 3. Merge
./perf-viz-merge --perf perf.data --viz trace.json -o merged.json

# 4. Validate with trace_processor
trace_processor_shell merged.json --query "SELECT count(*) FROM slice"

# 5. Basic JSON validation
python3 -c "import json; d=json.load(open('merged.json')); print(f'Events: {len(d[\"traceEvents\"])}')"
```

### Test Python script
```python
# test/generate_test_data.py
import time
from viztracer import VizTracer

def cpu_work(n):
    total = 0
    for i in range(n):
        total += i * i
    return total

def io_work():
    time.sleep(0.1)

def main():
    tracer = VizTracer(output_file="trace.json")
    tracer.start()
    for _ in range(5):
        cpu_work(100000)
        io_work()
    tracer.stop()
    tracer.save()

if __name__ == "__main__":
    main()
```
