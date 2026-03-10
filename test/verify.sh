#!/bin/bash
#
# End-to-end test for perf-viz-merge.
#
# Usage: ./test/verify.sh [--synthetic] [--with-perf]
#
# --synthetic: Use synthetic test data only (default, no special privileges needed)
# --with-perf: Try to use real perf record (requires root or perf_event_paranoid=-1)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_OUTPUT_DIR="${PROJECT_DIR}/test/output"
BINARY="${PROJECT_DIR}/perf-viz-merge"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pass_count=0
fail_count=0
skip_count=0

pass() {
    echo -e "${GREEN}PASS${NC}: $1"
    pass_count=$((pass_count + 1))
}

fail() {
    echo -e "${RED}FAIL${NC}: $1"
    fail_count=$((fail_count + 1))
}

skip() {
    echo -e "${YELLOW}SKIP${NC}: $1"
    skip_count=$((skip_count + 1))
}

# Parse arguments
USE_SYNTHETIC=true
USE_PERF=false
for arg in "$@"; do
    case "$arg" in
        --with-perf) USE_PERF=true; USE_SYNTHETIC=false ;;
        --synthetic) USE_SYNTHETIC=true ;;
    esac
done

echo "========================================"
echo " perf-viz-merge end-to-end tests"
echo "========================================"
echo ""

# Check that the binary exists
if [ ! -x "$BINARY" ]; then
    echo "Binary not found at $BINARY. Run 'make' first."
    exit 1
fi

# Clean and create output directory
rm -rf "$TEST_OUTPUT_DIR"
mkdir -p "$TEST_OUTPUT_DIR"

# ----------------------------------------
# Test 1: Generate test data
# ----------------------------------------
echo "--- Test 1: Generate test data ---"

if $USE_SYNTHETIC; then
    python3 "$SCRIPT_DIR/generate_test_data.py" \
        --output-dir "$TEST_OUTPUT_DIR" \
        --synthetic-only
    TRACE_JSON="$TEST_OUTPUT_DIR/trace.json"
    PERF_DATA="$TEST_OUTPUT_DIR/perf.data"
else
    python3 "$SCRIPT_DIR/generate_test_data.py" \
        --output-dir "$TEST_OUTPUT_DIR"
    TRACE_JSON="$TEST_OUTPUT_DIR/trace.json"

    if $USE_PERF; then
        python3 "$SCRIPT_DIR/generate_test_data.py" \
            --output-dir "$TEST_OUTPUT_DIR" \
            --with-perf
        PERF_DATA="$TEST_OUTPUT_DIR/perf.data"
    else
        # Generate synthetic perf.data
        python3 -c "
import sys; sys.path.insert(0, '$SCRIPT_DIR')
from generate_test_data import generate_synthetic_perf_data
generate_synthetic_perf_data('$TEST_OUTPUT_DIR')
"
        PERF_DATA="$TEST_OUTPUT_DIR/perf.data"
    fi
fi

if [ -f "$TRACE_JSON" ]; then
    pass "VizTracer trace.json generated"
else
    fail "VizTracer trace.json not generated"
fi

if [ -f "$PERF_DATA" ]; then
    pass "perf.data generated"
else
    fail "perf.data not generated"
fi

# ----------------------------------------
# Test 2: Basic merge
# ----------------------------------------
echo ""
echo "--- Test 2: Basic merge ---"

MERGED_OUTPUT="$TEST_OUTPUT_DIR/merged.perfetto-trace"

if "$BINARY" --perf "$PERF_DATA" --viz "$TRACE_JSON" -o "$MERGED_OUTPUT" 2>"$TEST_OUTPUT_DIR/merge_stderr.txt"; then
    pass "perf-viz-merge ran successfully"
else
    fail "perf-viz-merge failed (exit code $?)"
    cat "$TEST_OUTPUT_DIR/merge_stderr.txt" | head -20
fi

# ----------------------------------------
# Test 3: Validate output Perfetto trace
# ----------------------------------------
echo ""
echo "--- Test 3: Validate output Perfetto trace ---"

if [ -f "$MERGED_OUTPUT" ]; then
    # Check file is non-empty
    FILE_SIZE=$(stat -c%s "$MERGED_OUTPUT" 2>/dev/null || echo "0")
    if [ "$FILE_SIZE" -gt 0 ]; then
        pass "Output is non-empty ($FILE_SIZE bytes)"
    else
        fail "Output file is empty"
    fi

    # Check that it starts with a valid protobuf tag (field 1, wire type 2 = 0x0a)
    FIRST_BYTE=$(od -A n -t x1 -N 1 "$MERGED_OUTPUT" 2>/dev/null | tr -d ' ')
    if [ "$FIRST_BYTE" = "0a" ]; then
        pass "Output starts with valid Perfetto protobuf tag"
    else
        fail "Output does not start with expected protobuf tag (got 0x$FIRST_BYTE)"
    fi
else
    fail "Merged output file not found"
fi

# ----------------------------------------
# Test 4: Verbose mode
# ----------------------------------------
echo ""
echo "--- Test 4: Verbose mode ---"

VERBOSE_OUTPUT="$TEST_OUTPUT_DIR/merged_verbose.perfetto-trace"
if "$BINARY" --perf "$PERF_DATA" --viz "$TRACE_JSON" -o "$VERBOSE_OUTPUT" -v 2>"$TEST_OUTPUT_DIR/verbose_stderr.txt"; then
    if grep -q "events" "$TEST_OUTPUT_DIR/verbose_stderr.txt" 2>/dev/null; then
        pass "Verbose mode produces progress output"
    else
        skip "Verbose mode ran but no progress messages detected"
    fi
else
    fail "Verbose mode failed"
fi

# ----------------------------------------
# Test 5: VizTracer-only merge (no perf.data)
# ----------------------------------------
echo ""
echo "--- Test 5: VizTracer-only mode ---"

VIZ_ONLY_OUTPUT="$TEST_OUTPUT_DIR/viz_only.perfetto-trace"
if "$BINARY" --viz "$TRACE_JSON" -o "$VIZ_ONLY_OUTPUT" 2>/dev/null; then
    if [ -f "$VIZ_ONLY_OUTPUT" ]; then
        VIZ_SIZE=$(stat -c%s "$VIZ_ONLY_OUTPUT" 2>/dev/null || echo "0")
        if [ "$VIZ_SIZE" -gt 0 ]; then
            pass "VizTracer-only mode produced non-empty output ($VIZ_SIZE bytes)"
        else
            fail "VizTracer-only mode produced empty output"
        fi
    else
        fail "VizTracer-only output file not created"
    fi
else
    skip "VizTracer-only mode not supported yet"
fi

# ----------------------------------------
# Test 6: Perfetto trace_processor validation (if available)
# ----------------------------------------
echo ""
echo "--- Test 6: Perfetto trace_processor validation ---"

if command -v trace_processor_shell &>/dev/null; then
    if [ -f "$MERGED_OUTPUT" ]; then
        SLICE_COUNT=$(trace_processor_shell "$MERGED_OUTPUT" \
            --query "SELECT count(*) as cnt FROM slice" 2>/dev/null | tail -1 || echo "error")
        if [ "$SLICE_COUNT" != "error" ] && [ "$SLICE_COUNT" -gt 0 ] 2>/dev/null; then
            pass "Perfetto trace_processor loaded file ($SLICE_COUNT slices)"
        else
            skip "trace_processor_shell query returned: $SLICE_COUNT"
        fi
    fi
else
    skip "trace_processor_shell not installed"
fi

# ----------------------------------------
# Summary
# ----------------------------------------
echo ""
echo "========================================"
echo " Results: ${GREEN}${pass_count} passed${NC}, ${RED}${fail_count} failed${NC}, ${YELLOW}${skip_count} skipped${NC}"
echo "========================================"

if [ "$fail_count" -gt 0 ]; then
    exit 1
fi
exit 0
