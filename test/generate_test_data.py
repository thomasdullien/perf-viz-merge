#!/usr/bin/env python3
"""
Generate test data for perf-viz-merge.

This script:
1. Runs a small workload under VizTracer to produce trace.json
2. Optionally records perf data (requires root or perf_event_paranoid=-1)

Usage:
    python3 generate_test_data.py [--output-dir DIR] [--with-perf]
"""

import argparse
import os
import subprocess
import sys
import time
import threading


def cpu_work(n):
    """Do some CPU-bound work."""
    total = 0
    for i in range(n):
        total += i * i
    return total


def io_work(duration=0.05):
    """Simulate I/O-bound work."""
    time.sleep(duration)


def thread_worker(thread_id, iterations):
    """Worker function for multi-threaded test."""
    for i in range(iterations):
        cpu_work(50000)
        io_work(0.02)


def run_workload():
    """Run a mixed workload with multiple threads."""
    # Single-threaded phase
    for _ in range(3):
        cpu_work(100000)
        io_work(0.05)

    # Multi-threaded phase
    threads = []
    for i in range(3):
        t = threading.Thread(target=thread_worker, args=(i, 2))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    # Final single-threaded phase
    cpu_work(200000)


def generate_viztracer_json(output_dir):
    """Generate VizTracer trace JSON by running the workload."""
    try:
        from viztracer import VizTracer
    except ImportError:
        print("VizTracer not installed. Generating synthetic trace JSON instead.")
        return generate_synthetic_viztracer_json(output_dir)

    trace_path = os.path.join(output_dir, "trace.json")
    tracer = VizTracer(output_file=trace_path)
    tracer.start()
    run_workload()
    tracer.stop()
    tracer.save()
    print(f"VizTracer trace written to {trace_path}")
    return trace_path


def generate_synthetic_viztracer_json(output_dir):
    """Generate a synthetic Chrome Trace Format JSON file for testing.

    This creates a valid trace without requiring VizTracer to be installed.
    """
    import json

    trace_path = os.path.join(output_dir, "trace.json")
    pid = os.getpid()
    tid = pid  # main thread

    events = []
    ts = 1000000.0  # start at 1 second in microseconds

    # Generate nested function call events
    functions = [
        ("main", 500000),
        ("cpu_work", 100000),
        ("io_work", 50000),
        ("inner_loop", 10000),
    ]

    for iteration in range(5):
        # main function
        main_start = ts
        events.append({
            "ph": "B", "name": "main", "cat": "python",
            "ts": ts, "pid": pid, "tid": tid,
        })
        ts += 10

        for func_idx in range(3):
            # cpu_work
            events.append({
                "ph": "B", "name": "cpu_work", "cat": "python",
                "ts": ts, "pid": pid, "tid": tid,
                "args": {"iteration": iteration, "index": func_idx}
            })

            for inner in range(5):
                events.append({
                    "ph": "X", "name": "inner_loop", "cat": "python",
                    "ts": ts, "dur": 8000, "pid": pid, "tid": tid,
                    "args": {"i": inner}
                })
                ts += 10000

            events.append({
                "ph": "E", "name": "cpu_work", "cat": "python",
                "ts": ts, "pid": pid, "tid": tid,
            })
            ts += 100

            # io_work
            events.append({
                "ph": "X", "name": "io_work", "cat": "python",
                "ts": ts, "dur": 50000, "pid": pid, "tid": tid,
            })
            ts += 50100

        events.append({
            "ph": "E", "name": "main", "cat": "python",
            "ts": ts, "pid": pid, "tid": tid,
        })
        ts += 1000

    # Add some events on other threads
    for thread_offset in range(1, 4):
        other_tid = tid + thread_offset
        thread_ts = 1200000.0
        for i in range(3):
            events.append({
                "ph": "X", "name": f"thread_worker_{thread_offset}",
                "cat": "python",
                "ts": thread_ts, "dur": 80000,
                "pid": pid, "tid": other_tid,
                "args": {"iteration": i}
            })
            thread_ts += 100000

    trace = {
        "traceEvents": events,
        "viztracer_metadata": {
            "version": "test",
            "overflow": False,
        },
    }

    with open(trace_path, "w") as f:
        json.dump(trace, f)

    print(f"Synthetic trace written to {trace_path} ({len(events)} events)")
    return trace_path


def generate_perf_data(output_dir):
    """Record perf data for the workload (requires elevated privileges)."""
    perf_path = os.path.join(output_dir, "perf.data")
    script_path = os.path.abspath(__file__)

    # Check if we can use perf
    try:
        result = subprocess.run(
            ["perf", "stat", "--", "true"],
            capture_output=True, timeout=5
        )
        if result.returncode != 0:
            print("Warning: perf not available or insufficient permissions.")
            print("Generating synthetic perf.data instead.")
            return generate_synthetic_perf_data(output_dir)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        print("Warning: perf not found. Generating synthetic perf.data instead.")
        return generate_synthetic_perf_data(output_dir)

    cmd = [
        "perf", "record",
        "-e", "sched:sched_switch",
        "-e", "sched:sched_wakeup",
        "-k", "CLOCK_MONOTONIC",
        "-o", perf_path,
        "--", "python3", script_path, "--workload-only",
    ]

    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"perf record failed: {result.stderr}")
        print("Generating synthetic perf.data instead.")
        return generate_synthetic_perf_data(output_dir)

    print(f"perf data written to {perf_path}")
    return perf_path


def generate_synthetic_perf_data(output_dir):
    """Generate a minimal synthetic perf.data file for testing.

    This creates a valid perf.data file with the correct header structure
    and a few synthetic sample events. This is sufficient for testing the
    parser without requiring actual perf recording privileges.
    """
    import struct

    perf_path = os.path.join(output_dir, "perf.data")
    pid = os.getpid()

    # perf.data magic: "PERFILE2"
    MAGIC = 0x32454C4946524550

    # perf_event_attr for a tracepoint event
    # We'll create a minimal attr with the fields we need
    PERF_TYPE_TRACEPOINT = 1

    # sample_type flags
    PERF_SAMPLE_TID = 1 << 1
    PERF_SAMPLE_TIME = 1 << 2
    PERF_SAMPLE_CPU = 1 << 3
    PERF_SAMPLE_RAW = 1 << 10

    sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW

    # Build a minimal perf_event_attr (136 bytes for kernel 6.1)
    ATTR_SIZE = 136
    attr = bytearray(ATTR_SIZE)

    # type (u32) at offset 0
    struct.pack_into('<I', attr, 0, PERF_TYPE_TRACEPOINT)
    # size (u32) at offset 4
    struct.pack_into('<I', attr, 4, ATTR_SIZE)
    # config (u64) at offset 8 - tracepoint ID (arbitrary for test)
    struct.pack_into('<Q', attr, 8, 1)  # tracepoint ID 1
    # sample_type (u64) at offset 24
    struct.pack_into('<Q', attr, 24, sample_type)
    # sample_id_all flag is at bit 18 of a flags field at offset 48
    # For simplicity, set it
    struct.pack_into('<Q', attr, 48, 1 << 18)

    # Build sample events
    # PERF_RECORD_SAMPLE = 9
    PERF_RECORD_SAMPLE = 9
    PERF_RECORD_COMM = 3

    events_data = bytearray()

    # First, add a COMM event to map pid to a name
    comm_name = b"python3\x00"
    # COMM event: pid(u32) + tid(u32) + comm(string)
    comm_payload = struct.pack('<II', pid, pid) + comm_name
    # Pad to 8-byte alignment
    while len(comm_payload) % 8 != 0:
        comm_payload += b'\x00'
    # Add sample_id suffix (since sample_id_all is set): tid(u32+u32) + time(u64) + cpu(u32+u32)
    comm_sample_id = struct.pack('<IIQI I', pid, pid, 1000000000, 0, 0)
    comm_payload += comm_sample_id
    comm_header = struct.pack('<IHH', PERF_RECORD_COMM, 0,
                              8 + len(comm_payload))
    events_data += comm_header + comm_payload

    # Generate sample events (sched_switch-like)
    base_time_ns = 1000000000  # 1 second in nanoseconds
    for i in range(20):
        ts = base_time_ns + i * 10000000  # 10ms apart

        # Raw tracepoint data for sched_switch (simplified)
        # Fields: prev_comm(16) + prev_pid(i32) + prev_prio(i32) + prev_state(i64)
        #       + next_comm(16) + next_pid(i32) + next_prio(i32)
        raw_data = bytearray()
        prev_comm = b"python3\x00" + b'\x00' * 8  # 16 bytes
        next_comm = b"worker\x00" + b'\x00' * 9   # 16 bytes
        raw_data += prev_comm
        raw_data += struct.pack('<i', pid)         # prev_pid
        raw_data += struct.pack('<i', 120)         # prev_prio
        raw_data += struct.pack('<q', 1)           # prev_state (TASK_INTERRUPTIBLE)
        raw_data += next_comm
        raw_data += struct.pack('<i', pid + 1)     # next_pid
        raw_data += struct.pack('<i', 120)         # next_prio

        # Sample payload: tid(u32+u32) + time(u64) + cpu(u32+u32) + raw_size(u32) + raw_data
        cpu = i % 4
        sample_payload = struct.pack('<II', pid, pid)  # pid, tid
        sample_payload += struct.pack('<Q', ts)         # time
        sample_payload += struct.pack('<II', cpu, 0)    # cpu, reserved
        sample_payload += struct.pack('<I', len(raw_data))  # raw size
        sample_payload += bytes(raw_data)

        # Pad to 8-byte alignment
        while len(sample_payload) % 8 != 0:
            sample_payload += b'\x00'

        sample_header = struct.pack('<IHH', PERF_RECORD_SAMPLE, 0,
                                    8 + len(sample_payload))
        events_data += sample_header + sample_payload

    # Build EVENT_DESC feature section
    # Format: u32 nr_events, u32 attr_size, then per event:
    #   perf_event_attr (attr_size bytes) + u32 nr_ids +
    #   null-terminated name (padded to 4 bytes) + u64 ids[nr_ids]
    event_name = b"sched:sched_switch\x00"
    # Pad name to 4-byte alignment
    while len(event_name) % 4 != 0:
        event_name += b'\x00'

    event_desc = struct.pack('<II', 1, ATTR_SIZE)  # nr_events=1, attr_size
    event_desc += bytes(attr)                       # the attr
    event_desc += struct.pack('<I', 0)              # nr_ids=0
    event_desc += struct.pack('<I', len(event_name))  # str_size
    event_desc += event_name                        # event name

    # Build the file
    # Layout: header (104 bytes) | attrs_with_ids | data | feature_sections | event_desc_data
    HEADER_SIZE = 104

    # attrs section: attr + FileSection for IDs (16 bytes)
    # Each entry is attr_size + 16 bytes
    attrs_entry = bytes(attr) + struct.pack('<QQ', 0, 0)  # no IDs
    attrs_offset = HEADER_SIZE
    attrs_size = len(attrs_entry)

    data_offset = attrs_offset + attrs_size
    data_size = len(events_data)

    # Feature section pointers come after data
    # We have one feature: EVENT_DESC (bit 12)
    FEAT_EVENT_DESC = 12
    feature_sections_offset = data_offset + data_size
    # One FileSection (16 bytes) pointing to the event_desc data
    event_desc_data_offset = feature_sections_offset + 16  # after the section pointer
    feature_section = struct.pack('<QQ', event_desc_data_offset, len(event_desc))

    # flags: set bit 12 (EVENT_DESC) in flags[0]
    flags = [1 << FEAT_EVENT_DESC, 0, 0, 0]

    header = struct.pack('<QQQ',
                         MAGIC,       # magic
                         HEADER_SIZE, # size
                         ATTR_SIZE)   # attr_size
    # attrs section: offset + size
    header += struct.pack('<QQ', attrs_offset, attrs_size)
    # data section: offset + size
    header += struct.pack('<QQ', data_offset, data_size)
    # event_types section: offset + size (unused)
    header += struct.pack('<QQ', 0, 0)
    # flags (4 x u64)
    header += struct.pack('<QQQQ', *flags)

    assert len(header) == HEADER_SIZE

    with open(perf_path, 'wb') as f:
        f.write(header)
        f.write(attrs_entry)
        f.write(bytes(events_data))
        f.write(feature_section)
        f.write(event_desc)

    print(f"Synthetic perf.data written to {perf_path} ({20} sample events)")
    return perf_path


def main():
    parser = argparse.ArgumentParser(description="Generate test data for perf-viz-merge")
    parser.add_argument("--output-dir", default=".", help="Output directory")
    parser.add_argument("--with-perf", action="store_true",
                        help="Also generate perf.data (may require root)")
    parser.add_argument("--workload-only", action="store_true",
                        help="Only run the workload (used by perf record)")
    parser.add_argument("--synthetic-only", action="store_true",
                        help="Only generate synthetic data (no viztracer/perf needed)")
    args = parser.parse_args()

    if args.workload_only:
        run_workload()
        return

    os.makedirs(args.output_dir, exist_ok=True)

    if args.synthetic_only:
        generate_synthetic_viztracer_json(args.output_dir)
        generate_synthetic_perf_data(args.output_dir)
    else:
        generate_viztracer_json(args.output_dir)
        if args.with_perf:
            generate_perf_data(args.output_dir)


if __name__ == "__main__":
    main()
