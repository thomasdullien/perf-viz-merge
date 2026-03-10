# perf-viz-merge

A C++17 tool that merges Linux `perf.data` recordings and
[VizTracer](https://github.com/gaogaotiantian/viztracer) Python traces into a
single [Perfetto](https://ui.perfetto.dev) trace file.

The merged trace shows **Python call stacks**, **scheduler state** (on-cpu /
off-cpu per thread), **GIL contention** (acquire wait vs. held), and
**GPU/NCCL activity** (CUDA kernel launches, memory copies, synchronization,
collective operations) — all time-aligned on per-thread tracks.

![Perfetto screenshot placeholder](https://ui.perfetto.dev)

## Why?

When profiling multi-threaded Python + CUDA workloads, no single tool gives the
full picture. VizTracer shows Python-level call stacks but not why a thread
stalled. `perf` shows scheduler and hardware events but not Python function
names. This tool merges both into one timeline so you can see, for example, that
a `Tensor.to()` call blocked for 12 ms because the thread was waiting for the
GIL while `cuMemcpyHtoD` ran on another thread.

## Quick start

### Build

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt install libtraceevent-dev libsimdjson-dev libfmt-dev libelf-dev libdw-dev

make
```

A fully static binary (no runtime dependencies) can also be built:

```bash
make static
```

### Collect data

Data collection has two parts: a `perf record` for kernel/uprobe events, and a
VizTracer run for Python call stacks. Both must use `CLOCK_MONOTONIC` so their
timestamps align automatically.

#### 1. Set up uprobes (once per boot)

The setup script auto-detects library paths and installs perf probes for the
Python GIL, NVIDIA CUDA driver API, and NCCL:

```bash
sudo ./scripts/setup-uprobes.sh
```

Use `--no-gil`, `--no-gpu`, or `--no-nccl` to skip categories.
Use `--dry-run` to preview commands without executing.

#### 2. Record perf data

Record scheduler events and uprobes for your process:

```bash
sudo perf record \
    -e sched:sched_switch \
    -e sched:sched_wakeup \
    -e sched:sched_process_fork \
    -e 'python:*' \
    -e 'nvidia:*' \
    -e 'nccl:*' \
    -k CLOCK_MONOTONIC \
    -o perf.data \
    -p <PID>
```

Or wrap your command directly:

```bash
sudo perf record \
    -e sched:sched_switch \
    -e sched:sched_wakeup \
    -e sched:sched_process_fork \
    -e 'python:*' \
    -e 'nvidia:*' \
    -e 'nccl:*' \
    -k CLOCK_MONOTONIC \
    -o perf.data \
    -- python my_script.py
```

#### 3. Record VizTracer data

In a separate terminal (or as part of your script), run VizTracer:

```bash
viztracer --tracer_entries 20000000 -o trace.json my_script.py
```

Or programmatically:

```python
from viztracer import VizTracer

with VizTracer(tracer_entries=20000000, output_file="trace.json"):
    main()
```

### Merge

```bash
./perf-viz-merge \
    --perf perf.data \
    --viz trace.json \
    --filter-pid <PID> \
    -o merged.perfetto-trace \
    -v
```

Open `merged.perfetto-trace` in the [Perfetto UI](https://ui.perfetto.dev).

### Command-line options

```
Usage: perf-viz-merge [options]

Options:
  --perf <path>          Path to perf.data file
  --viz <path>           Path to VizTracer JSON file
  -o, --output <path>    Output file (default: merged.perfetto-trace)
  --time-offset <us>     Manual time offset in microseconds
  --filter-pid <pid>     Only include events for this PID
  --no-sched             Omit scheduler events
  --no-gil               Omit GIL tracking events
  --no-gpu               Omit GPU/NCCL events
  -v, --verbose          Print progress information
  -h, --help             Show this help
```

At least one of `--perf` or `--viz` is required. Both can be provided for a
merged trace, or either alone for format conversion.

## Track layout in Perfetto

Each thread gets up to four child tracks, grouped together:

| Track | Content |
|-------|---------|
| **sched** | on-cpu / off-cpu spans with sleep reason (S/D/T/etc.) |
| **GIL** | "GIL acquire" (waiting) and "GIL held" spans |
| **call stacks** | VizTracer Python function spans |
| **GPU** | CUDA and NCCL API call spans (launch, sync, memcpy, allreduce, ...) |

## How it works

- **perf.data parsing**: Standalone binary parser (no dependency on `perf
  script` or kernel headers). Decodes `sample_type` bitmasks, parses raw
  tracepoint data for `sched_switch`, `sched_wakeup`, and `sched_process_fork`.
- **VizTracer parsing**: Streams JSON via simdjson's ondemand API (memory-mapped,
  handles multi-GB files).
- **Clock alignment**: Auto-detects that both sources use `CLOCK_MONOTONIC` when
  timestamps are large absolute values with overlapping ranges. Falls back to
  start-time alignment otherwise.
- **Scheduler state machine**: Uses `sched_switch` for switch-OUT and
  `sched_wakeup` for switch-IN detection (the only reliable switch-IN signal
  with per-process recording).
- **GIL state machine**: `take_gil` / `take_gil_return` / `drop_gil` uprobe
  triplet, with idempotent handlers for automatic duplicate-probe deduplication.
- **GPU state machine**: Begin/end pairs for all CUDA driver API and NCCL
  calls, with open-span deduplication.
- **Perfetto output**: Hand-coded protobuf encoder with string interning and
  hierarchical track descriptors (process -> thread -> child tracks).

## Dependencies

| Library | Package | Purpose |
|---------|---------|---------|
| libtraceevent | `libtraceevent-dev` | Tracepoint format parsing |
| simdjson | `libsimdjson-dev` | VizTracer JSON streaming |
| fmt | `libfmt-dev` | String formatting |
| libelf | `libelf-dev` | ELF parsing |
| libdw | `libdw-dev` (elfutils) | DWARF debug info |

The static build (`make static`) vendors simdjson and fmt automatically.

## License

MIT
