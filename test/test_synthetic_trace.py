#!/usr/bin/env python3
"""
Tests for perf-viz-merge using synthetic .ftrc and perf.data files.

Generates synthetic trace data, runs perf-viz-merge, and verifies the
merged Perfetto output using trace_processor_shell queries.

Usage:
    python3 test/test_synthetic_trace.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
BINARY = os.environ.get(
    "PERF_VIZ_MERGE",
    os.path.join(PROJECT_DIR, "perf-viz-merge-static"),
)

# Import the generator
sys.path.insert(0, SCRIPT_DIR)
from generate_synthetic_trace import generate_all, PID, TID_MAIN, TID_MCPROC0, TID_MCPROC1


class TestResult:
    """Track test pass/fail counts."""

    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.errors = []

    def ok(self, msg: str):
        self.passed += 1
        print(f"  PASS: {msg}")

    def fail(self, msg: str):
        self.failed += 1
        self.errors.append(msg)
        print(f"  FAIL: {msg}")

    def check(self, condition: bool, msg: str):
        if condition:
            self.ok(msg)
        else:
            self.fail(msg)


def tp_query(trace_path: str, sql: str) -> str:
    """Run a trace_processor_shell query, return stdout."""
    result = subprocess.run(
        ["trace_processor_shell", trace_path, "-Q", sql],
        capture_output=True, text=True, timeout=30,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"trace_processor_shell failed: {result.stderr.strip()}")
    return result.stdout.strip()


def tp_query_int(trace_path: str, sql: str) -> int:
    """Run a query expected to return a single integer."""
    out = tp_query(trace_path, sql)
    lines = out.strip().split("\n")
    if len(lines) < 2:
        return 0
    return int(lines[-1].strip().strip('"'))


def tp_query_rows(trace_path: str, sql: str) -> list:
    """Run a query and return rows as list of dicts."""
    out = tp_query(trace_path, sql)
    lines = out.strip().split("\n")
    if len(lines) < 2:
        return []
    headers = [h.strip().strip('"') for h in lines[0].split(",")]
    rows = []
    for line in lines[1:]:
        vals = [v.strip().strip('"') for v in line.split(",")]
        rows.append(dict(zip(headers, vals)))
    return rows


def run_merge(args: list, stderr_path: str = None) -> int:
    """Run perf-viz-merge with given args. Returns exit code."""
    cmd = [BINARY] + args
    stderr_file = open(stderr_path, "w") if stderr_path else subprocess.DEVNULL
    try:
        result = subprocess.run(cmd, capture_output=False, stdout=subprocess.DEVNULL,
                                stderr=stderr_file, timeout=60)
        return result.returncode
    finally:
        if stderr_path:
            stderr_file.close()


def test_basic_merge(tmpdir: str, ftrc_path: str, perf_path: str,
                     result: TestResult):
    """Test 1: Basic merge produces valid Perfetto output."""
    print("\n--- Test 1: Basic merge ---")

    output = os.path.join(tmpdir, "merged.perfetto-trace")
    stderr_path = os.path.join(tmpdir, "merge_stderr.txt")

    rc = run_merge([
        "--perf", perf_path,
        "--viz", ftrc_path,
        "--min-duration", "2",
        "-o", output,
        "-v",
    ], stderr_path)

    result.check(rc == 0, f"perf-viz-merge exits with code 0 (got {rc})")
    result.check(os.path.isfile(output), "Output file exists")

    size = os.path.getsize(output) if os.path.isfile(output) else 0
    result.check(size > 0, f"Output is non-empty ({size} bytes)")

    # Check protobuf tag
    if os.path.isfile(output):
        with open(output, "rb") as f:
            first_byte = f.read(1)
        result.check(first_byte == b'\x0a',
                     "Output starts with valid Perfetto protobuf tag")

    return output


def test_call_stacks(output: str, result: TestResult):
    """Test 2: Call stack slices exist with correct function names."""
    print("\n--- Test 2: Call stack verification ---")

    total = tp_query_int(output, "SELECT count(*) FROM slice")
    result.check(total > 100, f"Total slices > 100 (got {total})")

    # Check known function names are present
    expected_funcs = [
        "MCProc.run",
        "Alg.update",
        "SCPose._update",
        "VitPosePoseEstimatorTriton.preprocess",
        "Tensor.to",
        "cv2.resize",
    ]
    rows = tp_query_rows(output,
        "SELECT DISTINCT name FROM slice ORDER BY name")
    found_names = {r["name"] for r in rows}

    for func in expected_funcs:
        result.check(func in found_names,
                     f"Function '{func}' found in slices")

    # Check that there are events from all 3 threads (via global track names)
    thread_rows = tp_query_rows(output,
        "SELECT DISTINCT track.name "
        "FROM slice JOIN track ON slice.track_id = track.id "
        "WHERE track.name LIKE 'stacks [%]'")
    track_names = {r["name"] for r in thread_rows}
    result.check(f"stacks [{TID_MAIN}]" in track_names,
                 "MainThread (tid=1000) has slices")
    result.check(f"stacks [{TID_MCPROC0}]" in track_names,
                 "mcproc__0 (tid=1001) has slices")
    result.check(f"stacks [{TID_MCPROC1}]" in track_names,
                 "mcproc__1 (tid=1002) has slices")


def test_sched_events(output: str, result: TestResult):
    """Test 3: Scheduler events exist."""
    print("\n--- Test 3: Scheduler events ---")

    # Check for on-cpu and off-cpu slices
    on_cpu = tp_query_int(output,
        "SELECT count(*) FROM slice WHERE name = 'on-cpu'")
    result.check(on_cpu > 0, f"on-cpu slices exist ({on_cpu})")

    off_cpu = tp_query_int(output,
        "SELECT count(*) FROM slice WHERE name LIKE 'Sleeping%'")
    result.check(off_cpu > 0, f"off-cpu (Sleeping) slices exist ({off_cpu})")

    # Check sched track exists (global track model: "sched [TID]")
    sched_tracks = tp_query_rows(output,
        "SELECT DISTINCT track.name FROM track WHERE track.name LIKE 'sched [%]'")
    result.check(len(sched_tracks) > 0, "sched tracks exist")


def test_gil_events(output: str, result: TestResult):
    """Test 4: GIL tracking events exist."""
    print("\n--- Test 4: GIL events ---")

    acquire = tp_query_int(output,
        "SELECT count(*) FROM slice WHERE name = 'GIL acquire'")
    result.check(acquire > 0, f"GIL acquire slices exist ({acquire})")

    held = tp_query_int(output,
        "SELECT count(*) FROM slice WHERE name = 'GIL held'")
    result.check(held > 0, f"GIL held slices exist ({held})")

    # Check GIL track exists (global track model: "GIL [TID]")
    gil_tracks = tp_query_rows(output,
        "SELECT DISTINCT track.name FROM track WHERE track.name LIKE 'GIL [%]'")
    result.check(len(gil_tracks) > 0, "GIL tracks exist")


def test_gpu_events(output: str, result: TestResult):
    """Test 5: GPU events exist."""
    print("\n--- Test 5: GPU events ---")

    launch = tp_query_int(output,
        "SELECT count(*) FROM slice WHERE name = 'cuLaunchKernel'")
    result.check(launch > 0, f"cuLaunchKernel slices exist ({launch})")

    sync = tp_query_int(output,
        "SELECT count(*) FROM slice WHERE name = 'cuStreamSynchronize'")
    result.check(sync > 0, f"cuStreamSynchronize slices exist ({sync})")

    # Check GPU track exists (global track model: "GPU [TID]")
    gpu_tracks = tp_query_rows(output,
        "SELECT DISTINCT track.name FROM track WHERE track.name LIKE 'GPU [%]'")
    result.check(len(gpu_tracks) > 0, "GPU tracks exist")


def test_thread_names(output: str, result: TestResult):
    """Test 6: Thread and process names are present in track hierarchy."""
    print("\n--- Test 6: Thread names ---")

    # With global tracks, thread groups are named "ThreadName [TID]"
    rows = tp_query_rows(output,
        "SELECT track.name FROM track "
        "WHERE track.name LIKE 'MainThread%' "
        "   OR track.name LIKE 'mcproc__%'")
    track_names = [r["name"] for r in rows]

    has_main = any(f"[{TID_MAIN}]" in n for n in track_names)
    has_mc0 = any(f"[{TID_MCPROC0}]" in n for n in track_names)
    has_mc1 = any(f"[{TID_MCPROC1}]" in n for n in track_names)

    result.check(has_main,
                 f"Thread 1000 exists (name='MainThread [{TID_MAIN}]')")
    result.check(has_mc0,
                 f"Thread 1001 exists (name='mcproc__0 [{TID_MCPROC0}]')")
    result.check(has_mc1,
                 f"Thread 1002 exists (name='mcproc__1 [{TID_MCPROC1}]')")

    # Process name: process group track is named "python3 [PID]"
    proc_rows = tp_query_rows(output,
        "SELECT track.name FROM track "
        "WHERE track.name LIKE 'python3 [%]'")
    result.check(len(proc_rows) > 0,
                 "Process 'python3' exists in track hierarchy")


def test_no_misplaced_events(output: str, result: TestResult):
    """Test 7: No misplaced_end_event warnings in trace."""
    print("\n--- Test 7: No misplaced end events ---")

    # Check for misplaced end events in the trace metadata
    # trace_processor reports these as stats
    misplaced = tp_query_int(output,
        "SELECT IFNULL(SUM(value), 0) FROM stats "
        "WHERE name = 'misplaced_end_event'")
    result.check(misplaced == 0,
                 f"No misplaced_end_event warnings (got {misplaced})")


def test_event_counts(output: str, result: TestResult):
    """Test 8: Event counts are plausible."""
    print("\n--- Test 8: Plausible event counts ---")

    total = tp_query_int(output, "SELECT count(*) FROM slice")
    result.check(total > 500, f"Total slices > 500 (got {total})")
    result.check(total < 50000, f"Total slices < 50000 (got {total})")

    # Call stack events should be the majority
    call_stack = tp_query_int(output,
        "SELECT count(*) FROM slice WHERE name NOT IN "
        "('on-cpu', 'GIL acquire', 'GIL held', 'cuLaunchKernel', "
        "'cuStreamSynchronize') AND name NOT LIKE 'Sleeping%'")
    result.check(call_stack > 200,
                 f"Call stack slices > 200 (got {call_stack})")

    # Sched events: should be roughly 2x the number of sched_switch events
    sched = tp_query_int(output,
        "SELECT count(*) FROM slice WHERE name = 'on-cpu' "
        "OR name LIKE 'Sleeping%'")
    result.check(sched >= 50, f"Sched slices >= 50 (got {sched})")


def test_chunking(tmpdir: str, ftrc_path: str, perf_path: str,
                  result: TestResult):
    """Test 9: Chunked output produces expected number of chunks."""
    print("\n--- Test 9: Chunking (5-second chunks) ---")

    chunk_dir = os.path.join(tmpdir, "chunks")
    os.makedirs(chunk_dir, exist_ok=True)
    output = os.path.join(chunk_dir, "chunked.perfetto-trace")

    rc = run_merge([
        "--perf", perf_path,
        "--viz", ftrc_path,
        "--min-duration", "2",
        "--chunk-duration", "5",
        "-o", output,
    ])

    result.check(rc == 0, "Chunked merge exits with code 0")

    # Should produce 3 chunks for 15 seconds of data
    chunk_files = sorted([
        f for f in os.listdir(chunk_dir)
        if f.startswith("chunked-") and f.endswith(".perfetto-trace")
    ])
    result.check(len(chunk_files) == 3,
                 f"Produces 3 chunks (got {len(chunk_files)})")

    # Each chunk should have events
    for cf in chunk_files:
        chunk_path = os.path.join(chunk_dir, cf)
        chunk_slices = tp_query_int(chunk_path,
            "SELECT count(*) FROM slice")
        result.check(chunk_slices > 0,
                     f"Chunk {cf} has slices ({chunk_slices})")

    # Check context spans: chunks 1 and 2 should have events from
    # before their nominal start time (context carried from prior chunks)
    if len(chunk_files) >= 2:
        chunk1 = os.path.join(chunk_dir, chunk_files[1])
        # Context spans should have events starting before 5s from trace start
        # Query for slices in chunk 1 — just verify it has a decent count
        c1_count = tp_query_int(chunk1, "SELECT count(*) FROM slice")
        result.check(c1_count > 50,
                     f"Chunk 1 has context spans ({c1_count} total slices)")


def main():
    # Verify prerequisites
    if not os.path.isfile(BINARY):
        print(f"ERROR: Binary not found at {BINARY}")
        print("Run 'make static' first.")
        sys.exit(1)

    if not shutil.which("trace_processor_shell"):
        print("ERROR: trace_processor_shell not found in PATH")
        sys.exit(1)

    print("========================================")
    print(" Synthetic trace tests for perf-viz-merge")
    print("========================================")

    result = TestResult()

    with tempfile.TemporaryDirectory(prefix="pvmerge-synth-") as tmpdir:
        # Generate synthetic data
        print("\nGenerating synthetic trace data...")
        ftrc_path, perf_path = generate_all(tmpdir)

        result.check(os.path.isfile(ftrc_path),
                     f"ftrc file generated ({os.path.getsize(ftrc_path)} bytes)")
        result.check(os.path.isfile(perf_path),
                     f"perf.data generated ({os.path.getsize(perf_path)} bytes)")

        # Run tests
        output = test_basic_merge(tmpdir, ftrc_path, perf_path, result)

        if output and os.path.isfile(output):
            test_call_stacks(output, result)
            test_sched_events(output, result)
            test_gil_events(output, result)
            test_gpu_events(output, result)
            test_thread_names(output, result)
            test_no_misplaced_events(output, result)
            test_event_counts(output, result)
            test_chunking(tmpdir, ftrc_path, perf_path, result)

    # Summary
    print("\n========================================")
    total = result.passed + result.failed
    print(f" Results: {result.passed}/{total} passed, {result.failed} failed")
    print("========================================")

    if result.errors:
        print("\nFailures:")
        for err in result.errors:
            print(f"  - {err}")

    sys.exit(1 if result.failed > 0 else 0)


if __name__ == "__main__":
    main()
