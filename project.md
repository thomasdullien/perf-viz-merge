# planning.md: High-Performance Perf/VizTracer Merger (C++)

## 1. Goal
Create a standalone C++ tool (perf-viz-merge) that merges a binary perf.data and a VizTracer .json file into a single, unified Chrome Trace Format JSON file compatible with Perfetto.
The goal is to allow to visualize the viztrace traces of the Python code, and overlay that with other tracks with the following information:
 - Scheduler data (when is a thread on-core, when is it taken off-core, and why?)
 - Arbitrary uprobes or kprobes that indicate time spans. This is to be used for multiple purposes:
  - uprobes on take_gil and similar Python functions to visualize who is taking the GIL and who is holding it
  - future uprobes inside the nvidia runtime to visualize when certain actions on the GPU are triggered.

Data collection has two steps: (1) set up uprobes, (2) run `perf record`.

### Setting up uprobes

The easiest way is to use the provided setup script:

```
sudo ./scripts/setup-uprobes.sh
```

This auto-detects library paths and installs probes for Python GIL, NVIDIA CUDA, and NCCL.
Use `--no-gil`, `--no-gpu`, `--no-nccl` to skip categories. See `--help` for all options.

Manual probe setup is also possible:

```
# Python GIL probes
LIB_PATH="/root/.local/share/uv/python/cpython-3.11.11-linux-x86_64-gnu/lib/libpython3.11.so.1.0"
perf probe -f -x $LIB_PATH python:take_gil=take_gil
perf probe -f -x $LIB_PATH python:take_gil_return=take_gil%return
perf probe -f -x $LIB_PATH python:drop_gil=drop_gil

# NVIDIA CUDA probes
CUDA_LIB="/usr/local/cuda/lib64/libcuda.so"
perf probe -f -x $CUDA_LIB nvidia:launch=cuLaunchKernel
perf probe -f -x $CUDA_LIB nvidia:launch_ret=cuLaunchKernel%return
perf probe -f -x $CUDA_LIB nvidia:stream_sync=cuStreamSynchronize
perf probe -f -x $CUDA_LIB nvidia:stream_sync_ret=cuStreamSynchronize%return
perf probe -f -x $CUDA_LIB nvidia:dev_sync=cuCtxSynchronize
perf probe -f -x $CUDA_LIB nvidia:dev_sync_ret=cuCtxSynchronize%return
perf probe -f -x $CUDA_LIB nvidia:event_sync=cuEventSynchronize
perf probe -f -x $CUDA_LIB nvidia:event_sync_ret=cuEventSynchronize%return
perf probe -f -x $CUDA_LIB nvidia:memcpy_htod=cuMemcpyHtoDAsync_v2
perf probe -f -x $CUDA_LIB nvidia:memcpy_htod_ret=cuMemcpyHtoDAsync_v2%return
perf probe -f -x $CUDA_LIB nvidia:memcpy_dtoh=cuMemcpyDtoHAsync_v2
perf probe -f -x $CUDA_LIB nvidia:memcpy_dtoh_ret=cuMemcpyDtoHAsync_v2%return
perf probe -f -x $CUDA_LIB nvidia:memcpy_dtod=cuMemcpyDtoDAsync_v2
perf probe -f -x $CUDA_LIB nvidia:memcpy_dtod_ret=cuMemcpyDtoDAsync_v2%return
perf probe -f -x $CUDA_LIB nvidia:memcpy_async=cuMemcpyAsync
perf probe -f -x $CUDA_LIB nvidia:memcpy_async_ret=cuMemcpyAsync%return
perf probe -f -x $CUDA_LIB nvidia:memcpy_peer=cuMemcpyPeerAsync
perf probe -f -x $CUDA_LIB nvidia:memcpy_peer_ret=cuMemcpyPeerAsync%return
perf probe -f -x $CUDA_LIB nvidia:malloc=cuMemAlloc_v2
perf probe -f -x $CUDA_LIB nvidia:malloc_ret=cuMemAlloc_v2%return
perf probe -f -x $CUDA_LIB nvidia:free=cuMemFree_v2
perf probe -f -x $CUDA_LIB nvidia:free_ret=cuMemFree_v2%return

# NCCL probes
NCCL_LIB="/usr/lib/x86_64-linux-gnu/libnccl.so"
perf probe -f -x $NCCL_LIB nccl:allreduce=ncclAllReduce
perf probe -f -x $NCCL_LIB nccl:allreduce_ret=ncclAllReduce%return
perf probe -f -x $NCCL_LIB nccl:broadcast=ncclBroadcast
perf probe -f -x $NCCL_LIB nccl:broadcast_ret=ncclBroadcast%return
perf probe -f -x $NCCL_LIB nccl:reducescatter=ncclReduceScatter
perf probe -f -x $NCCL_LIB nccl:reducescatter_ret=ncclReduceScatter%return
```

### Recording

```
sudo /opt/zystem/perf/linux-6.5/tools/perf/perf record -g \
  -e sched:sched_switch \
  -e sched:sched_process_fork \
  -e sched:sched_wakeup \
  -e sched:sched_stat_runtime \
  -e context-switches \
  -e 'python:*' \
  -e 'nvidia:*' \
  -e 'nccl:*' \
  -k CLOCK_MONOTONIC \
  -o perf.data \
  -- just dat run runsets/bb/SCRATCH/my-test-run
```

The "just" command kicks off the Python process in which viztracer is also used.
This can be replaced by any Python command or a viztracer command.

Key events:
- `sched:sched_switch` — thread switch-out (who left the CPU and why)
- `sched:sched_stat_runtime` — ~4ms heartbeat confirming thread is on-CPU
- `context-switches` — software event firing at exact switch-IN time
- `python:*` — GIL acquire/release uprobes
- `nvidia:*` — CUDA driver API uprobes (launch, sync, memcpy, alloc)
- `nccl:*` — NCCL collective uprobes

The `sched_stat_runtime` and `context-switches` events fire in the incoming task's
context, so they are captured by per-process (`-p PID`) recording even when the
switching-out task belongs to a different process. This fills the gap where
`sched_switch` alone only sees switch-OUT events for the target process.

## 2. Core Constraints
- Data Scale: 30M+ events (~1.5GB to 5GB of data).
- Memory Limit: Must stay under 32GB RAM. Do NOT load the full JSON into memory.
- Clock Alignment: Detect the T0 offset between perf (uptime-based) and viztracer (usually 0-based) and normalize.

## 3. Desiderata

The command takes two inputs: A .json trace created by viztracer, and the perf.data file with a recording
of the scheduler events, the Python GIL events, and the future Nvidia events (and possibly others, to be determined).

The program should parse the perf.data file using libperf and libtraceevent (parsing the perf script text output is
too unreliable and unstable) and resolve the addresses from the uprobes using locally-available debug information
back to function names if necessary.

## 4. Build Requirements
- Language: C++17 or C++20.
- Library: simdjson (Use the ondemand API for zero-copy streaming).
- Library: fmt or std::format for high-speed JSON string generation.
- Use "make" as build system if possible


## 5. Verify that everything works

In order to verify that everything works, a small example Python file should be created, instrumented
with viztracer, and the relevant perf command should be issued.

The result should be the two input files (.json and perf.data). The tool should merge the two, and
the perfetto trace_processor command line tool should be used to verify that loading the merged file
works and that the correct number of events appear in the file.
 
