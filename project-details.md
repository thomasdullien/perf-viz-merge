# project-details.md: Design & Implementation of perf-viz-merge

## 1. Overview

**perf-viz-merge** is a C++17 command-line tool that merges:
- A **perf.data** binary file (scheduler events, GIL uprobes, NVIDIA/NCCL uprobes)
- A **VizTracer JSON** file (Python function-level tracing)

into a single **Perfetto protobuf trace** (preferred) or Chrome Trace Format JSON
file, viewable in the [Perfetto UI](https://ui.perfetto.dev).

The Perfetto native format is preferred over Chrome Trace JSON because it supports
hierarchical track grouping — per-thread child tracks for scheduler state, GIL
contention, and GPU activity appear nested under the main thread track.

### Key challenges solved
- 15M+ perf events + 7M+ VizTracer events merged in under a minute
- Two independent clock domains (perf CLOCK_MONOTONIC ns, VizTracer CLOCK_MONOTONIC μs)
  auto-aligned without user intervention
- Per-process perf recording (`-p PID`) only captures switch-OUT via `sched_switch`;
  switch-IN is detected via `sched_wakeup` tracepoint parsing
- Duplicate uprobe events (perf creates `probe` + `probe_1`) deduplicated automatically
- Standalone perf.data binary parser — no dependency on kernel source or `perf script`

---

## 2. Architecture

```
┌─────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  perf.data   │────▶│  PerfDataReader  │────▶│                 │
│  (binary)    │     │  (standalone     │     │                 │
└─────────────┘     │   parser +       │     │   MergeEngine   │──▶  output
                     │   libtraceevent) │     │   (state machines│    (Perfetto proto
┌─────────────┐     └──────────────────┘     │    + merge-sort) │     or Chrome JSON)
│  trace.json  │────▶┌──────────────────┐    │                 │
│  (VizTracer) │     │  VizJsonReader   │────▶│                 │
└─────────────┘     │  (simdjson       │     └────────┬────────┘
                     │   ondemand)      │              │
                     └──────────────────┘     ┌────────▼────────┐
                                              │  OutputWriter   │
                                    ┌─────────┴─────────┐      │
                                    │                   │      │
                              PerfettoWriter      TraceWriter   │
                              (native proto)      (JSON)        │
                                    └─────────┬─────────┘      │
                                              │  ClockAligner   │
                                              └─────────────────┘
```

### Components

| Component | File(s) | Responsibility |
|-----------|---------|---------------|
| **PerfDataReader** | `perf_data_reader.h/cpp`, `perf_data_format.h` | Parses perf.data binary format; decodes sample fields by `sample_type` bitmask; parses raw tracepoint data for sched_switch, sched_wakeup, sched_process_fork; classifies events by name and attr type |
| **VizJsonReader** | `viz_json_reader.h/cpp` | Streams VizTracer JSON via simdjson ondemand API (memory-mapped); yields one event at a time |
| **ClockAligner** | `clock_aligner.h` (header-only) | Auto-detects whether both sources use CLOCK_MONOTONIC (large absolute timestamps with overlapping ranges → offset=0) or different bases (align start times); supports manual `--time-offset` override |
| **MergeEngine** | `merge_engine.h/cpp` | Merge-sorts perf and VizTracer events by timestamp; implements scheduler, GIL, and GPU state machines; emits output events via OutputWriter interface |
| **PerfettoWriter** | `perfetto_writer.h/cpp` | Hand-coded protobuf encoder producing native Perfetto traces; manages track hierarchy (process → thread → child tracks for sched/GIL/GPU); string interning for event names and categories |
| **TraceWriter** | `trace_writer.h/cpp` | Streaming Chrome Trace Format JSON writer using fmt::format |
| **OutputWriter** | `output_writer.h` | Abstract interface implemented by both writers: `write_complete`, `write_begin`, `write_end`, `write_instant`, `write_metadata`, `write_viz_event`, `finalize` |

---

## 3. Event Types and State Machines

### 3.1 Supported perf event types

Defined in `event_types.h`:

| Event Type | Source | Purpose |
|---|---|---|
| `SchedSwitch` | `sched:sched_switch` | Thread switch-OUT detection (raw data: prev_tid, next_tid, prev_state, comm) |
| `SchedWakeup` | `sched:sched_wakeup` | Thread switch-IN detection (raw data: target_tid) |
| `SchedFork` | `sched:sched_process_fork` | New thread/process creation |
| `SchedStatRuntime` | `sched:sched_stat_runtime` | ~4ms on-CPU heartbeat (supplementary to wakeup) |
| `ContextSwitch` | `context-switches` | Software event, fires on switch-out (filtered as echo of sched_switch) |
| `TakeGil` | `python:take_gil` | GIL acquisition start |
| `TakeGilReturn` | `python:take_gil_return` | GIL acquired (uprobe return probe) |
| `DropGil` | `python:drop_gil` | GIL released |
| `NvidiaLaunch/Return` | `nvidia:launch` | CUDA kernel launch (cuLaunchKernel) |
| `NvidiaStreamSync/Return` | `nvidia:stream_sync` | cuStreamSynchronize |
| `NvidiaDeviceSync/Return` | `nvidia:dev_sync` | cuCtxSynchronize |
| `NvidiaEventSync/Return` | `nvidia:event_sync` | cuEventSynchronize |
| `NvidiaMemcpy*/Return` | `nvidia:memcpy_*` | cuMemcpy variants (HtoD, DtoH, DtoD, Async, Peer) |
| `NvidiaMalloc/Free/Return` | `nvidia:malloc/free` | cuMemAlloc_v2 / cuMemFree_v2 |
| `NcclAllReduce/Return` | `nccl:allreduce` | ncclAllReduce |
| `NcclBroadcast/Return` | `nccl:broadcast` | ncclBroadcast |
| `NcclReduceScatter/Return` | `nccl:reducescatter` | ncclReduceScatter |

### 3.2 Scheduler state machine

The fundamental challenge with per-process perf recording (`-p PID`) is that
`sched:sched_switch` only fires in the context of the outgoing task. We only
see switch-OUT events for our threads; switch-IN from external tasks is invisible.

**Switch-IN detection** uses `sched:sched_wakeup`:
- The wakeup tracepoint fires when a thread is placed on the run queue
- Raw tracepoint data is parsed to extract `target_tid`
- When a wakeup targets a thread currently marked off-cpu, the off-cpu span
  is closed and the thread transitions to on-cpu

**Supplementary events**:
- `sched_stat_runtime`: ~4ms periodic heartbeat from `update_curr()`. In practice,
  98%+ of these fire as pre-switch ticks (3-7μs before sched_switch), NOT as
  periodic heartbeats. Still useful as fallback switch-in detection when wakeup
  events are missing.
- `context-switches`: Software event that fires on switch-OUT with per-process
  recording. Filtered by a 10μs minimum gap to reject switch-out echoes.

**Per-thread state**:
```cpp
struct SchedState {
    uint64_t last_event_ns;  // timestamp of last state transition
    bool on_cpu;              // current state
    int64_t off_cpu_reason;   // prev_state from sched_switch
    int32_t last_cpu;
};
```

**Output**: Complete (`X`) events on synthetic sched tracks:
- `"on-cpu"` spans (category `"sched"`) with CPU info
- Off-cpu spans (category `"sched.off-cpu"`) with sleep reason (S/D/T/etc.)

### 3.3 GIL state machine

Tracks GIL contention per-thread using three probes:
- `take_gil` → begin "GIL acquire" span (waiting for GIL)
- `take_gil_return` → end "GIL acquire", begin "GIL held" span
- `drop_gil` → end "GIL held" span

**Deduplication**: perf installs duplicate probes (`python:take_gil` + `python:take_gil_1`).
Handlers are idempotent — skip if already in acquire/held state.

### 3.4 GPU/NCCL state machine

All NVIDIA and NCCL events use begin/end (B/E) pairs on a separate GPU track.
Deduplication via `gpu_open_` set per thread — skip begin if span already open
for that event name, skip end if no matching open span.

---

## 4. Track Organization in Perfetto

Each thread gets up to 4 tracks, grouped together via per-thread sort indices:

| Track | Synthetic TID Offset | Sort Order | Content |
|---|---|---|---|
| Sched | `tid + 200000000` | 0 (first) | on-cpu / off-cpu spans, wakeup instants |
| GIL | `tid + 100000000` | 1 | GIL acquire / GIL held spans |
| Call Stacks | `tid` (real) | 2 | VizTracer Python function spans |
| GPU | `tid + 300000000` | 3 (last) | CUDA/NCCL API call spans |

**Perfetto writer track hierarchy**: Each synthetic track is created as a child
track of the parent thread track, with a descriptive name (e.g., "mcproc__0 [sched]").
This ensures all tracks for the same thread are visually grouped in the Perfetto UI.

The `PerfettoWriter` uses hand-coded protobuf encoding with:
- UUID-based track identification (unique per track)
- String interning for event names and categories (reduces trace size)
- Process and thread descriptor packets for proper grouping

---

## 5. Parsing perf.data

### 5.1 Standalone binary parser

The `PerfDataReader` implements a standalone perf.data parser without depending
on kernel source or `libperf`. It handles:

1. **File header**: magic (`PERFILE2`), section offsets, feature flags
2. **Attrs section**: `perf_event_attr` array defining sample_type bitmasks
3. **Feature sections**: `EVENT_DESC` for event names, `CMDLINE`, etc.
4. **Data section**: Stream of `perf_event` records

### 5.2 Sample decoding

Each `PERF_RECORD_SAMPLE` is decoded field-by-field based on the `sample_type`
bitmask from the corresponding attr. Fields are read in strict bit order:

```
PERF_SAMPLE_IDENTIFIER → PERF_SAMPLE_IP → PERF_SAMPLE_TID →
PERF_SAMPLE_TIME → PERF_SAMPLE_ADDR → PERF_SAMPLE_ID →
PERF_SAMPLE_STREAM_ID → PERF_SAMPLE_CPU → ... → PERF_SAMPLE_RAW
```

### 5.3 Raw tracepoint data parsing

For tracepoint events, the `PERF_SAMPLE_RAW` field contains the tracepoint
payload. The parser handles three tracepoint formats:

**sched_switch** (64 bytes with common header):
- `prev_comm[16]`, `prev_pid(4)`, `prev_prio(4)`, `prev_state(8)`
- `next_comm[16]`, `next_pid(4)`, `next_prio(4)`

**sched_wakeup** (36 bytes with common header):
- `comm[16]`, `pid(4)` (target TID), `prio(4)`, `target_cpu(4)`

**sched_process_fork** (48 bytes with common header):
- `parent_comm[16]`, `parent_pid(4)`, `child_comm[16]`, `child_pid(4)`

All layouts handle both with and without the 8-byte common header prefix.

### 5.4 Event classification

Events are classified by name (from `EVENT_DESC` feature section) with fallback
by attr type/config. For example, `context-switches` is identified either by
name or by `PERF_TYPE_SOFTWARE + config=3`.

---

## 6. Clock Alignment

Both perf and VizTracer typically use `CLOCK_MONOTONIC`. The auto-detection
heuristic checks:

1. Are both first-timestamps large absolute values (> 1e9 μs ≈ 17min uptime)?
2. Is the difference between them less than the combined recording duration?

If both conditions hold → same clock, offset = 0.
Otherwise → different clock bases, align start times.

This avoids the pitfall where perf starts recording before VizTracer (e.g.,
22 seconds earlier) and the heuristic incorrectly concludes "different clocks."

---

## 7. CLI Interface

```
Usage: perf-viz-merge [options]

Options:
  --perf <path>          Path to perf.data file
  --viz <path>           Path to VizTracer JSON file
  -o, --output <path>    Output file (default: auto based on format)
  --format <fmt>         Output format: json or perfetto (default: json)
  --time-offset <us>     Manual time offset in microseconds
  --filter-pid <pid>     Only include events for this PID
  --no-sched             Omit scheduler events
  --no-gil               Omit GIL tracking events
  --no-gpu               Omit GPU/NCCL events
  -v, --verbose          Print progress information
  -h, --help             Show this help
```

At least one of `--perf` or `--viz` is required. Both can be provided for a
merged trace, or either alone for conversion.

---

## 8. Build System

### Dependencies

| Dependency | Package | Purpose |
|---|---|---|
| libtraceevent | `libtraceevent-dev` | Tracepoint format parsing (used during perf.data reading) |
| simdjson | `libsimdjson-dev` or vendored | VizTracer JSON streaming parser |
| fmt | `libfmt-dev` or vendored | Fast string formatting |
| libelf | `libelf-dev` | ELF parsing |
| libdw | Part of elfutils | DWARF debug info |

### Build targets

```bash
make                # Build perf-viz-merge (dynamic linking)
make static         # Build fully static binary (vendors simdjson + fmt)
make test           # Run test suite
make clean          # Remove object files and binaries
make distclean      # Remove everything including vendor/
```

### Directory structure

```
perftrace/
├── project.md                 # High-level overview and data collection guide
├── project-details.md         # This file — design and implementation details
├── Makefile
├── src/
│   ├── main.cpp               # CLI parsing, orchestration
│   ├── event_types.h          # PerfEvent, VizEvent, PerfEventType enum
│   ├── perf_data_reader.h/cpp # Standalone perf.data binary parser
│   ├── perf_data_format.h     # perf.data binary format structs
│   ├── viz_json_reader.h/cpp  # simdjson streaming VizTracer reader
│   ├── clock_aligner.h        # Clock alignment (header-only)
│   ├── merge_engine.h/cpp     # State machines, merge logic
│   ├── output_writer.h        # Abstract output interface
│   ├── perfetto_writer.h/cpp  # Native Perfetto protobuf output
│   ├── trace_writer.h/cpp     # Chrome Trace JSON output
│   ├── dump_thread_events.cpp # Diagnostic: per-thread event dump
│   └── dump_gpu_events.cpp    # Diagnostic: GPU event analysis
├── scripts/
│   └── setup-uprobes.sh       # Auto-detect libs and install perf probes
└── test/
    ├── generate_test_data.py   # Synthetic VizTracer trace generator
    └── verify.sh               # End-to-end test harness
```

---

## 9. Diagnostic Tools

### dump_thread_events

Dumps all perf events for a specific thread in timestamp order. Essential for
debugging the scheduler state machine:

```bash
g++ -std=c++17 -O2 [...] -o dump_thread_events src/dump_thread_events.cpp src/perf_data_reader.o [libs]
./dump_thread_events real-data/perf.data <tid> [max_events]
```

Shows the interleaving of SchedSwitch, SchedWakeup, SchedStatRuntime,
ContextSwitch, TakeGil, and other events for a single thread.

### dump_gpu_events

Shows which threads have GPU events and their counts:

```bash
./dump_gpu_events real-data/perf.data
```

---

## 10. Known Limitations and Design Decisions

### Per-process recording limitations
- `sched_switch` only captures switch-OUT for the target process
- `context-switches` fires only on switch-OUT (not switch-IN as documented)
- `sched_stat_runtime` is 98%+ pre-switch ticks, not periodic heartbeats
- `sched_wakeup` is the primary switch-IN indicator, but requires raw data parsing

### Duplicate uprobe handling
perf may install duplicate probes (`python:take_gil` + `python:take_gil_1`).
All handlers are idempotent to prevent duplicate spans.

### Perfetto vs Chrome Trace JSON
Perfetto native format is preferred because:
- Child tracks group under parent threads (sched/GIL/GPU next to call stacks)
- String interning reduces trace file size
- Better handling of large traces in the Perfetto UI

Chrome Trace JSON is still supported but track grouping is less reliable
(Perfetto UI sorts tracks by numeric TID, scattering synthetic tracks).

### Memory budget
| Data | Size |
|---|---|
| 15M perf events × ~48 bytes | ~700 MB |
| VizTracer mmap (5 GB file) | ~5 GB virtual (paged) |
| State machine maps | < 100 MB |
| **Total peak** | **~1-6 GB physical** |
