#!/bin/bash
#
# Setup uprobes for perf-viz-merge tracing.
#
# Configures perf probes on:
#   - Python GIL functions (take_gil, drop_gil)
#   - NVIDIA CUDA driver API (kernel launch, sync, memcpy, alloc)
#   - NCCL collectives (allreduce, broadcast, reducescatter)
#
# Usage:
#   sudo ./scripts/setup-uprobes.sh [options]
#
# Options:
#   --python-lib <path>    Path to libpython (auto-detected if not set)
#   --cuda-lib <path>      Path to libcuda.so (default: auto-detected)
#   --nccl-lib <path>      Path to libnccl.so (default: auto-detected)
#   --no-gil               Skip Python GIL probes
#   --no-gpu               Skip NVIDIA CUDA probes
#   --no-nccl              Skip NCCL probes
#   --no-alloc             Skip GPU memory allocation probes
#   --clean                Remove all probes instead of adding them
#   --list                 List currently installed probes
#   --dry-run              Show commands without executing
#
# Environment:
#   PERF                   Path to perf executable (default: perf)

set -euo pipefail

# Defaults
PERF="${PERF:-perf}"
PYTHON_LIB=""
CUDA_LIB=""
NCCL_LIB=""
DO_GIL=true
DO_GPU=true
DO_NCCL=true
DO_ALLOC=true
DO_CLEAN=false
DO_LIST=false
DRY_RUN=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --python-lib)  PYTHON_LIB="$2"; shift 2 ;;
        --cuda-lib)    CUDA_LIB="$2"; shift 2 ;;
        --nccl-lib)    NCCL_LIB="$2"; shift 2 ;;
        --no-gil)      DO_GIL=false; shift ;;
        --no-gpu)      DO_GPU=false; shift ;;
        --no-nccl)     DO_NCCL=false; shift ;;
        --no-alloc)    DO_ALLOC=false; shift ;;
        --clean)       DO_CLEAN=true; shift ;;
        --list)        DO_LIST=true; shift ;;
        --dry-run)     DRY_RUN=true; shift ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^#//' | sed 's/^ //'
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

run() {
    if $DRY_RUN; then
        echo "[dry-run] $*"
    else
        echo "+ $*"
        "$@" || echo "  WARNING: command failed (probe may already exist or symbol not found)"
    fi
}

# --- List existing probes ---
if $DO_LIST; then
    echo "=== Installed perf probes ==="
    $PERF probe -l 2>/dev/null || echo "(none)"
    exit 0
fi

# --- Clean all probes ---
if $DO_CLEAN; then
    echo "Removing all perf probes..."
    for probe in ($PERF probe -l 2>/dev/null | awk '{print $1}' | sed 's/:$//'); do
        run $PERF probe -d "$probe"
    done
    echo "Done."
    exit 0
fi

# --- Auto-detect library paths ---
find_lib() {
    local name="$1"
    # Try common locations
    local candidates=(
        "/usr/lib/x86_64-linux-gnu/$name"
        "/usr/lib64/$name"
        "/usr/local/lib/$name"
        "/usr/local/cuda/lib64/$name"
        "/usr/lib/$name"
    )
    # Also search with ldconfig
    local ldconfig_path
    ldconfig_path=$(ldconfig -p 2>/dev/null | grep "$name" | head -1 | sed 's/.*=> //' || true)
    if [[ -n "$ldconfig_path" && -f "$ldconfig_path" ]]; then
        echo "$ldconfig_path"
        return 0
    fi
    for path in "${candidates[@]}"; do
        # Expand glob
        local matches=($path*)
        for m in "${matches[@]}"; do
            if [[ -f "$m" ]]; then
                echo "$m"
                return 0
            fi
        done
    done
    return 1
}

find_python_lib() {
    # Try to find libpython via python3 itself
    local pylib
    pylib=$(python3 -c "
import sysconfig, os
paths = [
    sysconfig.get_config_var('LIBDIR'),
    sysconfig.get_config_var('INSTSONAME'),
    sysconfig.get_config_var('LDLIBRARY'),
]
libdir = sysconfig.get_config_var('LIBDIR') or '/usr/lib'
ldlib = sysconfig.get_config_var('INSTSONAME') or sysconfig.get_config_var('LDLIBRARY') or ''
if ldlib:
    full = os.path.join(libdir, ldlib)
    if os.path.exists(full):
        print(full)
    else:
        # Try without the full path
        print(ldlib)
" 2>/dev/null || true)

    if [[ -n "$pylib" && -f "$pylib" ]]; then
        echo "$pylib"
        return 0
    fi

    # Fallback: search common locations
    find_lib "libpython3" && return 0

    # Try uv-managed pythons
    local uv_path
    uv_path=$(find /root/.local/share/uv/python/ -name 'libpython*.so*' -type f 2>/dev/null | head -1 || true)
    if [[ -n "$uv_path" ]]; then
        echo "$uv_path"
        return 0
    fi

    return 1
}

echo "========================================"
echo " perf-viz-merge: uprobe setup"
echo "========================================"
echo ""

# ==========================================
# Python GIL probes
# ==========================================
if $DO_GIL; then
    echo "--- Python GIL probes ---"

    if [[ -z "$PYTHON_LIB" ]]; then
        PYTHON_LIB=$(find_python_lib) || true
    fi

    if [[ -z "$PYTHON_LIB" || ! -f "$PYTHON_LIB" ]]; then
        echo "WARNING: Cannot find libpython. Skipping GIL probes."
        echo "  Use --python-lib <path> to specify manually."
    else
        echo "Using Python library: $PYTHON_LIB"

        # When a thread starts trying to acquire the GIL
        run $PERF probe -f -x "$PYTHON_LIB" python:take_gil=take_gil

        # When a thread successfully acquires the GIL
        run $PERF probe -f -x "$PYTHON_LIB" python:take_gil_return=take_gil%return

        # When a thread releases the GIL
        run $PERF probe -f -x "$PYTHON_LIB" python:drop_gil=drop_gil
    fi
    echo ""
fi

# ==========================================
# NVIDIA CUDA probes
# ==========================================
if $DO_GPU; then
    echo "--- NVIDIA CUDA probes ---"

    if [[ -z "$CUDA_LIB" ]]; then
        CUDA_LIB=$(find_lib "libcuda.so") || true
    fi

    if [[ -z "$CUDA_LIB" || ! -f "$CUDA_LIB" ]]; then
        echo "WARNING: Cannot find libcuda.so. Skipping CUDA probes."
        echo "  Use --cuda-lib <path> to specify manually."
    else
        echo "Using CUDA library: $CUDA_LIB"

        # Kernel launch
        run $PERF probe -f -x "$CUDA_LIB" nvidia:launch=cuLaunchKernel
        run $PERF probe -f -x "$CUDA_LIB" nvidia:launch_ret=cuLaunchKernel%return

        # Stream synchronize
        run $PERF probe -f -x "$CUDA_LIB" nvidia:stream_sync=cuStreamSynchronize
        run $PERF probe -f -x "$CUDA_LIB" nvidia:stream_sync_ret=cuStreamSynchronize%return

        # Device synchronize (try both driver and runtime API names)
        run $PERF probe -f -x "$CUDA_LIB" nvidia:dev_sync=cuCtxSynchronize
        run $PERF probe -f -x "$CUDA_LIB" nvidia:dev_sync_ret=cuCtxSynchronize%return

        # Event synchronize
        run $PERF probe -f -x "$CUDA_LIB" nvidia:event_sync=cuEventSynchronize
        run $PERF probe -f -x "$CUDA_LIB" nvidia:event_sync_ret=cuEventSynchronize%return

        # Memory transfers
        run $PERF probe -f -x "$CUDA_LIB" nvidia:memcpy_htod=cuMemcpyHtoDAsync_v2
        run $PERF probe -f -x "$CUDA_LIB" nvidia:memcpy_htod_ret=cuMemcpyHtoDAsync_v2%return

        run $PERF probe -f -x "$CUDA_LIB" nvidia:memcpy_dtoh=cuMemcpyDtoHAsync_v2
        run $PERF probe -f -x "$CUDA_LIB" nvidia:memcpy_dtoh_ret=cuMemcpyDtoHAsync_v2%return

        run $PERF probe -f -x "$CUDA_LIB" nvidia:memcpy_dtod=cuMemcpyDtoDAsync_v2
        run $PERF probe -f -x "$CUDA_LIB" nvidia:memcpy_dtod_ret=cuMemcpyDtoDAsync_v2%return

        run $PERF probe -f -x "$CUDA_LIB" nvidia:memcpy_async=cuMemcpyAsync
        run $PERF probe -f -x "$CUDA_LIB" nvidia:memcpy_async_ret=cuMemcpyAsync%return

        run $PERF probe -f -x "$CUDA_LIB" nvidia:memcpy_peer=cuMemcpyPeerAsync
        run $PERF probe -f -x "$CUDA_LIB" nvidia:memcpy_peer_ret=cuMemcpyPeerAsync%return

        # Memory allocation (optional, can be noisy)
        if $DO_ALLOC; then
            run $PERF probe -f -x "$CUDA_LIB" nvidia:malloc=cuMemAlloc_v2
            run $PERF probe -f -x "$CUDA_LIB" nvidia:malloc_ret=cuMemAlloc_v2%return

            run $PERF probe -f -x "$CUDA_LIB" nvidia:free=cuMemFree_v2
            run $PERF probe -f -x "$CUDA_LIB" nvidia:free_ret=cuMemFree_v2%return
        fi
    fi
    echo ""
fi

# ==========================================
# NCCL probes
# ==========================================
if $DO_NCCL; then
    echo "--- NCCL probes ---"

    if [[ -z "$NCCL_LIB" ]]; then
        NCCL_LIB=$(find_lib "libnccl.so") || true
    fi

    if [[ -z "$NCCL_LIB" || ! -f "$NCCL_LIB" ]]; then
        echo "NOTE: Cannot find libnccl.so. Skipping NCCL probes."
        echo "  Use --nccl-lib <path> to specify manually."
    else
        echo "Using NCCL library: $NCCL_LIB"

        run $PERF probe -f -x "$NCCL_LIB" nccl:allreduce=ncclAllReduce
        run $PERF probe -f -x "$NCCL_LIB" nccl:allreduce_ret=ncclAllReduce%return

        run $PERF probe -f -x "$NCCL_LIB" nccl:broadcast=ncclBroadcast
        run $PERF probe -f -x "$NCCL_LIB" nccl:broadcast_ret=ncclBroadcast%return

        run $PERF probe -f -x "$NCCL_LIB" nccl:reducescatter=ncclReduceScatter
        run $PERF probe -f -x "$NCCL_LIB" nccl:reducescatter_ret=ncclReduceScatter%return
    fi
    echo ""
fi

# ==========================================
# Summary
# ==========================================
echo "========================================"
echo " Installed probes:"
echo "========================================"
$PERF probe -l 2>/dev/null || echo "(none)"
echo ""
echo "To record, run:"
echo "  sudo $PERF record -g \\"
echo "    -e sched:sched_switch \\"
echo "    -e sched:sched_wakeup \\"
echo "    -e sched:sched_stat_runtime \\"
echo "    -e context-switches \\"

# Build the -e flags for installed probes
for probe in $($PERF probe -l 2>/dev/null | awk '{print $1}' | sed 's/:$//'); do
    group=$(echo "$probe" | cut -d: -f1)
    echo "    -e '${group}:*' \\"
done | sort -u

echo "    -k CLOCK_MONOTONIC \\"
echo "    -o perf.data \\"
echo "    -- <your-command>"
