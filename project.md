# planning.md: High-Performance Perf/VizTracer Merger (C++)

## 1. Goal
Create a standalone C++ tool (perf-viz-merge) that merges a binary perf.data and a VizTracer .json file into a single, unified Chrome Trace Format JSON file compatible with Perfetto.
The goal is to allow to visualize the viztrace traces of the Python code, and overlay that with other tracks with the following information:
 - Scheduler data (when is a thread on-core, when is it taken off-core, and why?)
 - Arbitrary uprobes or kprobes that indicate time spans. This is to be used for multiple purposes:
  - uprobes on take_gil and similar Python functions to visualize who is taking the GIL and who is holding it
  - future uprobes inside the nvidia runtime to visualize when certain actions on the GPU are triggered.

Data will be collected as follows:

```
sudo /opt/zystem/perf/linux-6.5/tools/perf/perf record -g \
  -e sched:sched_switch \
  -e sched:sched_process_fork \
  -e sched:sched_wakeup \
  -e 'python:*' \
  -k CLOCK_MONOTONIC \
  -o perf.data \
  -- just dat run runsets/bb/SCRATCH/my-test-run
```

The "just" command kicks off the Python process in which viztracer is also used.
This can be replaced by any Python command or a viztracer command.

The '-e python:\*' part enables specific uprobes defined as follows:

```
LIB_PATH="/root/.local/share/uv/python/cpython-3.11.11-linux-x86_64-gnu/lib/libpython3.11.so.1.0"

# 1. When a thread starts trying to acquire the GIL
perf probe -f -x $LIB_PATH python:take_gil=take_gil

# 2. When a thread successfully acquires the GIL
perf probe -f -x $LIB_PATH python:take_gil_return=take_gil%return

# 3. When a thread releases the GIL
perf probe -f -x $LIB_PATH python:drop_gil=drop_gil
```

The future nvidia probes are likely going to be defined as follows:

```
# Path to your driver library (example path)
CUDA_LIB="/usr/local/cuda/lib64/libcuda.so"

# 1. Capture when a Kernel is launched
perf probe -f -x $CUDA_LIB nvidia:launch=cuLaunchKernel

# 2. Capture when the CPU starts waiting for a stream
perf probe -f -x $CUDA_LIB nvidia:sync_start=cuStreamSynchronize

# 3. Capture when that wait finishes
perf probe -f -x $CUDA_LIB nvidia:sync_end=cuStreamSynchronize%return
```

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
 
