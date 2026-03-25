#!/usr/bin/env python3
"""
Test that chunked output preserves call stack context across chunk boundaries.

Creates a synthetic trace with known boundary-crossing spans, chunks it,
and verifies each chunk contains the expected context spans.
"""

import json
import os
import subprocess
import sys
import tempfile

BINARY = os.environ.get(
    "PERF_VIZ_MERGE",
    os.path.join(os.path.dirname(__file__), "..", "perf-viz-merge-static"),
)

# Base timestamp in microseconds (arbitrary large value like real traces)
BASE_TS = 1_000_000_000.0


def make_event(name, ts_us, dur_us, pid=1000, tid=1000):
    return {
        "ph": "X",
        "name": name,
        "ts": ts_us,
        "dur": dur_us,
        "pid": pid,
        "tid": tid,
    }


def make_metadata(name, pid=1000, tid=1000):
    return {
        "ph": "M",
        "name": "thread_name",
        "pid": pid,
        "tid": tid,
        "args": {"name": name},
    }


def create_test_trace(path):
    """Create a trace with events that deliberately cross chunk boundaries.

    Timeline (with 5-second chunks):
        Chunk 0: 0s - 5s
        Chunk 1: 5s - 10s
        Chunk 2: 10s - 15s

    Events:
        main()       : 0s  - 14s  (crosses chunk 0->1->2)
        outer()      : 1s  - 12s  (crosses chunk 0->1->2)
        inner_a()    : 2s  - 4s   (entirely in chunk 0)
        inner_b()    : 6s  - 8s   (entirely in chunk 1)
        inner_c()    : 11s - 13s  (entirely in chunk 2)
        short_span() : 4.9s - 5.1s (crosses chunk 0->1 boundary)
        isolated()   : 7s  - 7.5s (entirely in chunk 1, no parent crossing)
    """
    events = [
        make_metadata("TestThread"),
        make_event("main", BASE_TS + 0e6, 14e6),           # 0s - 14s
        make_event("outer", BASE_TS + 1e6, 11e6),          # 1s - 12s
        make_event("inner_a", BASE_TS + 2e6, 2e6),         # 2s - 4s
        make_event("inner_b", BASE_TS + 6e6, 2e6),         # 6s - 8s
        make_event("inner_c", BASE_TS + 11e6, 2e6),        # 11s - 13s
        make_event("short_span", BASE_TS + 4.9e6, 0.2e6),  # 4.9s - 5.1s
        make_event("isolated", BASE_TS + 7e6, 0.5e6),      # 7s - 7.5s
    ]

    with open(path, "w") as f:
        json.dump({"traceEvents": events}, f)


def read_trace_events(path):
    """Read events from a Perfetto trace using perf-viz-merge in a round-trip.

    Since we can't easily parse Perfetto protobuf, we verify by examining
    the verbose output and event counts. For deeper verification, we use
    a helper: convert the chunked Perfetto back to JSON isn't trivial,
    so we rely on the tool's own verbose output.
    """
    # We can't directly read Perfetto protobuf, but we can check file size
    # and use the tool's verbose output for verification.
    return os.path.getsize(path)


def run_merge(args):
    """Run perf-viz-merge and return (returncode, stdout, stderr)."""
    cmd = [BINARY] + args
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.returncode, result.stdout, result.stderr


def test_context_spans_present():
    """Test that context spans are carried across chunk boundaries."""
    with tempfile.TemporaryDirectory(prefix="pvz_chunk_test_") as tmpdir:
        trace_path = os.path.join(tmpdir, "trace.json")
        create_test_trace(trace_path)

        # Chunk into 5-second intervals
        rc, _, stderr = run_merge([
            "--viz", trace_path,
            "--chunk-duration", "5",
            "-o", os.path.join(tmpdir, "out.perfetto-trace"),
            "-v",
        ])
        assert rc == 0, f"merge failed: {stderr}"

        # Verify chunk files exist
        chunks = sorted(f for f in os.listdir(tmpdir) if f.startswith("out-"))
        assert len(chunks) == 3, f"Expected 3 chunks, got {len(chunks)}: {chunks}"

        # Check verbose output for context spans
        assert "context spans carried" in stderr, \
            f"Expected context span messages in output:\n{stderr}"

        # Chunk 1 (5s-10s) should have context spans from main() and outer()
        # which both started before 5s and extend past 5s
        lines = stderr.split("\n")
        chunk1_ctx = [l for l in lines if "context spans" in l]
        assert len(chunk1_ctx) >= 1, "Expected at least 1 context span report"

        # Verify all chunk files are non-empty
        for chunk_name in chunks:
            size = os.path.getsize(os.path.join(tmpdir, chunk_name))
            assert size > 0, f"Chunk {chunk_name} is empty"

        print(f"  PASS: {len(chunks)} chunks created with context spans")
        return True


def test_context_spans_correct_count():
    """Test that the correct number of context spans are generated."""
    with tempfile.TemporaryDirectory(prefix="pvz_chunk_count_") as tmpdir:
        trace_path = os.path.join(tmpdir, "trace.json")
        create_test_trace(trace_path)

        rc, _, stderr = run_merge([
            "--viz", trace_path,
            "--chunk-duration", "5",
            "-o", os.path.join(tmpdir, "out.perfetto-trace"),
            "-v",
        ])
        assert rc == 0, f"merge failed: {stderr}"

        # Parse context span counts from verbose output
        # Chunk 1 (5-10s): main() and outer() and short_span() cross the boundary
        #   main:       starts 0s, ends 14s -> crosses into chunk 1
        #   outer:      starts 1s, ends 12s -> crosses into chunk 1
        #   short_span: starts 4.9s, ends 5.1s -> crosses into chunk 1
        # = 3 context spans for chunk 1
        #
        # Chunk 2 (10-15s): main() and outer() cross the boundary
        #   main:       starts 0s, ends 14s -> crosses into chunk 2
        #   outer:      starts 1s, ends 12s -> crosses into chunk 2
        # = 2 context spans for chunk 2

        lines = stderr.split("\n")
        ctx_lines = [l for l in lines if "context spans carried" in l]

        # Extract counts
        counts = []
        for l in ctx_lines:
            # Format: "  N context spans carried from previous chunk"
            parts = l.strip().split()
            counts.append(int(parts[0]))

        assert len(counts) == 2, f"Expected 2 context reports (chunks 1,2), got {len(counts)}"
        assert counts[0] == 3, f"Chunk 1 should have 3 context spans (main, outer, short_span), got {counts[0]}"
        assert counts[1] == 2, f"Chunk 2 should have 2 context spans (main, outer), got {counts[1]}"

        print(f"  PASS: Correct context span counts: chunk1={counts[0]}, chunk2={counts[1]}")
        return True


def test_no_context_spans_first_chunk():
    """Test that the first chunk never has context spans."""
    with tempfile.TemporaryDirectory(prefix="pvz_chunk_first_") as tmpdir:
        trace_path = os.path.join(tmpdir, "trace.json")
        create_test_trace(trace_path)

        rc, _, stderr = run_merge([
            "--viz", trace_path,
            "--chunk-duration", "5",
            "-o", os.path.join(tmpdir, "out.perfetto-trace"),
            "-v",
        ])
        assert rc == 0, f"merge failed: {stderr}"

        # "Writing chunk 0" should not be followed by "context spans"
        lines = stderr.split("\n")
        for i, line in enumerate(lines):
            if "Writing chunk 0" in line:
                # Next non-empty line should NOT mention context spans
                for j in range(i + 1, min(i + 3, len(lines))):
                    assert "context spans" not in lines[j], \
                        f"First chunk should never have context spans: {lines[j]}"
                break

        print("  PASS: No context spans in first chunk")
        return True


def test_total_events_include_context():
    """Test that chunked output with context has MORE total events than without."""
    with tempfile.TemporaryDirectory(prefix="pvz_chunk_total_") as tmpdir:
        trace_path = os.path.join(tmpdir, "trace.json")
        create_test_trace(trace_path)

        # Single output (no chunking)
        rc, _, _ = run_merge([
            "--viz", trace_path,
            "-o", os.path.join(tmpdir, "single.perfetto-trace"),
        ])
        assert rc == 0
        single_size = os.path.getsize(os.path.join(tmpdir, "single.perfetto-trace"))

        # Chunked output
        rc, _, _ = run_merge([
            "--viz", trace_path,
            "--chunk-duration", "5",
            "-o", os.path.join(tmpdir, "chunked.perfetto-trace"),
        ])
        assert rc == 0

        # Total chunked size should be larger (due to context spans + per-chunk headers)
        total_chunked = sum(
            os.path.getsize(os.path.join(tmpdir, f))
            for f in os.listdir(tmpdir) if f.startswith("chunked-")
        )
        assert total_chunked > single_size, \
            f"Chunked total ({total_chunked}) should be larger than single ({single_size})"

        print(f"  PASS: Chunked total ({total_chunked}B) > single ({single_size}B)")
        return True


def test_single_chunk_no_context():
    """Test that when trace fits in one chunk, no context spans are created."""
    with tempfile.TemporaryDirectory(prefix="pvz_chunk_single_") as tmpdir:
        trace_path = os.path.join(tmpdir, "trace.json")
        create_test_trace(trace_path)

        # Use a chunk duration longer than the trace (15s > 14s)
        rc, _, stderr = run_merge([
            "--viz", trace_path,
            "--chunk-duration", "20",
            "-o", os.path.join(tmpdir, "out.perfetto-trace"),
            "-v",
        ])
        assert rc == 0, f"merge failed: {stderr}"
        assert "context spans" not in stderr, "Single chunk should have no context spans"

        chunks = [f for f in os.listdir(tmpdir) if f.startswith("out-")]
        assert len(chunks) == 1, f"Expected 1 chunk, got {len(chunks)}"

        print("  PASS: Single chunk, no context spans")
        return True


if __name__ == "__main__":
    if not os.path.exists(BINARY):
        print(f"Binary not found: {BINARY}")
        print("Build with: make static")
        sys.exit(1)

    tests = [
        ("Context spans present", test_context_spans_present),
        ("Context span counts correct", test_context_spans_correct_count),
        ("No context spans in first chunk", test_no_context_spans_first_chunk),
        ("Total events include context", test_total_events_include_context),
        ("Single chunk no context", test_single_chunk_no_context),
    ]

    passed = 0
    failed = 0
    for name, test_fn in tests:
        try:
            print(f"--- {name} ---")
            test_fn()
            passed += 1
        except AssertionError as e:
            print(f"  FAIL: {e}")
            failed += 1
        except Exception as e:
            print(f"  ERROR: {e}")
            failed += 1

    print(f"\n{'=' * 40}")
    print(f"Results: {passed} passed, {failed} failed")
    print(f"{'=' * 40}")
    sys.exit(1 if failed > 0 else 0)
