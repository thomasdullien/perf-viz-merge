# perf-viz-merge

Merges Linux perf.data, fasttracer .ftrc files, and metric CSVs into
Perfetto native protobuf traces.

## Coding style

- Use named types (typedefs / `using`) for distinct ID domains instead of
  bare `uint64_t` / `int64_t`. For example, use `TrackUUID` for Perfetto
  track UUIDs rather than raw `uint64_t`. This makes the code self-documenting
  and catches mix-ups at review time.
- C++17 (the static build uses `-std=c++17`).
- Build with `make static` (vendors simdjson and fmt).
- Run tests with `make test` (requires `trace_processor_shell` in PATH).
- Keep the `OutputWriter` interface abstract — `PerfettoWriter` and
  `TraceWriter` implement it, plus `ChunkingWriter` wraps it.

## Build

```bash
make clean && make static   # static binary
make test                   # run all tests (8 base + 40 synthetic)
```

## Test data

`test/generate_synthetic_trace.py` creates a 15-second .ftrc + perf.data
with sched/GIL/GPU events. Use it for fast iteration instead of the 87GB
production perf.data.
