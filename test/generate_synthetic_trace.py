#!/usr/bin/env python3
"""
Generate synthetic .ftrc and perf.data test files for perf-viz-merge.

Produces ~15 seconds of plausible trace data exercising:
  - ftrc v3 call stacks (3 threads, nested functions, up to 10-15 depth)
  - perf.data sched_switch, GIL (take_gil / take_gil_return / drop_gil),
    and GPU (nvidia:launch / nvidia:launch_ret / nvidia:stream_sync /
    nvidia:stream_sync_ret) events

Usage:
    python3 generate_synthetic_trace.py [--output-dir DIR]
"""

import argparse
import os
import struct
from typing import List, Tuple


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
PID = 1000
TID_MAIN = 1000
TID_MCPROC0 = 1001
TID_MCPROC1 = 1002

THREAD_NAMES = {0: "MainThread", 1: "mcproc__0", 2: "mcproc__1"}
THREAD_TIDS = {0: TID_MAIN, 1: TID_MCPROC0, 2: TID_MCPROC1}
PROCESS_NAME = "python3"

# 15-second trace starting at ~1 second after boot (realistic CLOCK_MONOTONIC)
BASE_TS_NS = 1_000_000_000  # 1 second in ns
DURATION_S = 15

# Function names used in the ftrc trace
FUNCTION_NAMES = [
    # index 1-based
    "MCProc.run",                          # 1
    "State.process",                       # 2
    "Alg.update",                          # 3
    "SCPose._update",                      # 4
    "VitPosePoseEstimatorTriton.preprocess",  # 5
    "Tensor.to",                           # 6
    "cv2.resize",                          # 7
    "SCNumberIdentifier._update",          # 8
    "torch.inference_mode",                # 9
    "numpy.array",                         # 10
    "MCProc._run_stage",                   # 11
    "State.trigger_algs",                  # 12
    "DataRequest.get",                     # 13
    "Alg.send_data",                       # 14
    "json.loads",                          # 15
]


# =========================================================================
# FTRC v3 generation
# =========================================================================

def _build_string_table(names: List[str]) -> bytes:
    """Build ftrc string table: [uint32 len][char data[len]] per entry, 1-indexed."""
    buf = bytearray()
    for name in names:
        encoded = name.encode("utf-8")
        buf += struct.pack("<I", len(encoded))
        buf += encoded
    return bytes(buf)


def _make_ftrc_event(ts_delta_us: int, func_id: int, tid_idx: int,
                     is_exit: bool = False, is_c: bool = False) -> bytes:
    """Create a 12-byte BinaryEvent."""
    flags = 0
    if is_exit:
        flags |= 0x80  # FT_FLAG_EXIT
    if is_c:
        flags |= 0x01  # FT_FLAG_C_FUNCTION
    return struct.pack("<IIBBH", ts_delta_us, func_id, tid_idx, flags, 0)


def _generate_call_stack_events(tid_idx: int, start_us: int,
                                rng_state: list) -> List[bytes]:
    """Generate a burst of nested call events for one thread.

    Returns a list of 12-byte BinaryEvent structs.
    """
    events = []

    # Simple LCG for deterministic pseudo-random numbers
    def rand_int(lo, hi):
        rng_state[0] = (rng_state[0] * 1103515245 + 12345) & 0x7FFFFFFF
        return lo + rng_state[0] % (hi - lo + 1)

    ts = start_us

    # Pattern: MCProc.run -> State.process -> Alg.update -> ...
    # We'll create 3 nested invocations per burst at varying depths
    outer_funcs = [
        (1, 11),  # MCProc.run -> MCProc._run_stage
        (2, 12),  # State.process -> State.trigger_algs
    ]
    inner_funcs = [
        (3, 4),   # Alg.update -> SCPose._update
        (3, 8),   # Alg.update -> SCNumberIdentifier._update
    ]
    leaf_funcs = [5, 6, 7, 9, 10, 13, 14, 15]

    for outer_a, outer_b in outer_funcs:
        # Entry outer_a
        events.append(_make_ftrc_event(ts, outer_a, tid_idx))
        ts += 1  # 1us later

        # Entry outer_b
        events.append(_make_ftrc_event(ts, outer_b, tid_idx))
        ts += 1

        for inner_a, inner_b in inner_funcs:
            # Entry inner_a
            events.append(_make_ftrc_event(ts, inner_a, tid_idx))

            # Same-timestamp child (tests depth sorting)
            events.append(_make_ftrc_event(ts, inner_b, tid_idx))
            ts += 1

            # A few leaf calls
            depth = rand_int(2, 5)
            leaf_stack = []
            for _ in range(depth):
                fid = leaf_funcs[rand_int(0, len(leaf_funcs) - 1)]
                events.append(_make_ftrc_event(ts, fid, tid_idx))
                leaf_stack.append(fid)
                ts += rand_int(0, 2)

            # Some more nested leaves
            for _ in range(rand_int(1, 3)):
                fid = leaf_funcs[rand_int(0, len(leaf_funcs) - 1)]
                events.append(_make_ftrc_event(ts, fid, tid_idx))
                ts += rand_int(5, 50)
                events.append(_make_ftrc_event(ts, fid, tid_idx, is_exit=True))
                ts += 1

            # Exit leaf stack
            for fid in reversed(leaf_stack):
                events.append(_make_ftrc_event(ts, fid, tid_idx, is_exit=True))
                ts += rand_int(0, 1)

            # Exit inner_b, inner_a
            ts += rand_int(10, 100)
            events.append(_make_ftrc_event(ts, inner_b, tid_idx, is_exit=True))
            ts += 1
            events.append(_make_ftrc_event(ts, inner_a, tid_idx, is_exit=True))
            ts += rand_int(5, 20)

        # Exit outer_b, outer_a
        events.append(_make_ftrc_event(ts, outer_b, tid_idx, is_exit=True))
        ts += 1
        events.append(_make_ftrc_event(ts, outer_a, tid_idx, is_exit=True))
        ts += rand_int(50, 200)

    return events


def generate_ftrc(output_dir: str) -> str:
    """Generate a .ftrc v3 file with 3 threads, 15 seconds of events."""
    ftrc_path = os.path.join(output_dir, "trace.ftrc")

    string_table_data = _build_string_table(FUNCTION_NAMES)
    num_strings = len(FUNCTION_NAMES)
    num_threads = 3

    # Generate events for each thread across 15 seconds
    all_events: List[bytes] = []

    for tid_idx in range(num_threads):
        rng = [tid_idx * 7919 + 42]  # deterministic seed per thread
        # Generate bursts every ~500ms across 15 seconds
        for burst_i in range(30):
            start_us = burst_i * 500_000 + tid_idx * 50_000  # stagger threads
            if start_us >= DURATION_S * 1_000_000:
                break
            burst_events = _generate_call_stack_events(tid_idx, start_us, rng)
            all_events.extend(burst_events)

    num_events = len(all_events)

    # Build the header
    # BufferHeaderV3 layout:
    #   magic(4) + version(4) + pid(4) + num_strings(4) + base_ts_ns(8) +
    #   num_events(4) + num_threads(1) + pad1(1) + pad2(2) +
    #   thread_table(256*8) + thread_names(256*64) + process_name(64) +
    #   string_table_offset(4) + events_offset(4)
    HEADER_SIZE = (4 + 4 + 4 + 4 + 8 + 4 + 1 + 1 + 2 +
                   256 * 8 + 256 * 64 + 64 + 4 + 4)
    # = 4+4+4+4+8+4+4 + 2048 + 16384 + 64 + 8 = 18536

    string_table_offset = HEADER_SIZE
    events_offset = string_table_offset + len(string_table_data)

    # Pack header
    header = bytearray()
    header += struct.pack("<I", 0x43525446)  # magic FTRC
    header += struct.pack("<I", 3)            # version 3
    header += struct.pack("<I", PID)
    header += struct.pack("<I", num_strings)
    header += struct.pack("<q", BASE_TS_NS)
    header += struct.pack("<I", num_events)
    header += struct.pack("<B", num_threads)
    header += struct.pack("<B", 0)   # pad1
    header += struct.pack("<H", 0)   # pad2

    # Thread table: 256 x uint64
    thread_table = bytearray(256 * 8)
    for idx, tid in THREAD_TIDS.items():
        struct.pack_into("<Q", thread_table, idx * 8, tid)
    header += thread_table

    # Thread names: 256 x 64 bytes
    thread_names = bytearray(256 * 64)
    for idx, name in THREAD_NAMES.items():
        encoded = name.encode("utf-8")[:63]
        offset = idx * 64
        thread_names[offset:offset + len(encoded)] = encoded
    header += thread_names

    # Process name: 64 bytes
    proc_name = bytearray(64)
    encoded = PROCESS_NAME.encode("utf-8")[:63]
    proc_name[:len(encoded)] = encoded
    header += proc_name

    # String table offset and events offset
    header += struct.pack("<I", string_table_offset)
    header += struct.pack("<I", events_offset)

    assert len(header) == HEADER_SIZE, f"Header size {len(header)} != {HEADER_SIZE}"

    # Write file
    events_data = b"".join(all_events)

    with open(ftrc_path, "wb") as f:
        f.write(bytes(header))
        f.write(string_table_data)
        f.write(events_data)

    print(f"ftrc v3 written to {ftrc_path} "
          f"({num_events} events, {num_strings} strings, {num_threads} threads)")
    return ftrc_path


# =========================================================================
# perf.data generation (extends the pattern from generate_test_data.py)
# =========================================================================

# perf.data constants
PERF_MAGIC = 0x32454C4946524550
PERF_TYPE_TRACEPOINT = 1
PERF_SAMPLE_TID = 1 << 1
PERF_SAMPLE_TIME = 1 << 2
PERF_SAMPLE_ID = 1 << 6
PERF_SAMPLE_CPU = 1 << 7
PERF_SAMPLE_RAW = 1 << 10
PERF_RECORD_SAMPLE = 9
PERF_RECORD_COMM = 3
PERF_RECORD_FORK = 7
ATTR_FLAG_SAMPLE_ID_ALL = 1 << 18
FEAT_EVENT_DESC = 12
HEADER_SIZE = 104
ATTR_SIZE = 136


def _pad_to(data: bytearray, alignment: int) -> bytearray:
    """Pad data to alignment boundary."""
    while len(data) % alignment != 0:
        data += b'\x00'
    return data


def _make_attr(tracepoint_id: int, sample_type: int) -> bytearray:
    """Create a perf_event_attr (ATTR_SIZE bytes)."""
    attr = bytearray(ATTR_SIZE)
    struct.pack_into('<I', attr, 0, PERF_TYPE_TRACEPOINT)  # type
    struct.pack_into('<I', attr, 4, ATTR_SIZE)              # size
    struct.pack_into('<Q', attr, 8, tracepoint_id)          # config
    struct.pack_into('<Q', attr, 24, sample_type)           # sample_type
    struct.pack_into('<Q', attr, 48, ATTR_FLAG_SAMPLE_ID_ALL)  # flags
    return attr


def _make_sample(pid: int, tid: int, ts_ns: int, cpu: int,
                 raw_data: bytes, sample_id: int,
                 sample_type: int) -> bytes:
    """Create a PERF_RECORD_SAMPLE record."""
    # Payload fields in ABI order:
    #   TID: pid(u32) + tid(u32)
    #   TIME: u64
    #   ID: u64  (if PERF_SAMPLE_ID is set)
    #   CPU: u32 + u32
    #   RAW: u32 size + data
    # Note: field order follows the ABI (perf_event_open man page):
    #   IDENTIFIER, IP, TID, TIME, ADDR, ID, STREAM_ID, CPU, ..., RAW
    payload = bytearray()
    if sample_type & PERF_SAMPLE_TID:
        payload += struct.pack('<II', pid, tid)
    if sample_type & PERF_SAMPLE_TIME:
        payload += struct.pack('<Q', ts_ns)
    if sample_type & PERF_SAMPLE_ID:
        payload += struct.pack('<Q', sample_id)
    if sample_type & PERF_SAMPLE_CPU:
        payload += struct.pack('<II', cpu, 0)
    if sample_type & PERF_SAMPLE_RAW:
        payload += struct.pack('<I', len(raw_data))
        payload += raw_data

    payload = _pad_to(payload, 8)

    header = struct.pack('<IHH', PERF_RECORD_SAMPLE, 0, 8 + len(payload))
    return bytes(header) + bytes(payload)


def _make_comm(pid: int, tid: int, name: str, ts_ns: int,
               sample_type: int, sample_id: int) -> bytes:
    """Create a PERF_RECORD_COMM record."""
    comm_name = name.encode("utf-8") + b'\x00'
    payload = bytearray(struct.pack('<II', pid, tid) + comm_name)
    payload = _pad_to(payload, 8)
    # sample_id suffix (since sample_id_all is set):
    # Fields in sample_id order: TID, TIME, ID, CPU
    suffix = bytearray()
    if sample_type & PERF_SAMPLE_TID:
        suffix += struct.pack('<II', pid, tid)
    if sample_type & PERF_SAMPLE_TIME:
        suffix += struct.pack('<Q', ts_ns)
    if sample_type & PERF_SAMPLE_ID:
        suffix += struct.pack('<Q', sample_id)
    if sample_type & PERF_SAMPLE_CPU:
        suffix += struct.pack('<II', 0, 0)
    payload += suffix

    header = struct.pack('<IHH', PERF_RECORD_COMM, 0, 8 + len(payload))
    return bytes(header) + bytes(payload)


def _make_fork(pid: int, ppid: int, tid: int, ptid: int,
               ts_ns: int, sample_type: int, sample_id: int) -> bytes:
    """Create a PERF_RECORD_FORK record."""
    payload = bytearray()
    payload += struct.pack('<II', pid, ppid)   # pid, ppid
    payload += struct.pack('<II', tid, ptid)   # tid, ptid
    payload += struct.pack('<Q', ts_ns)        # time
    # sample_id suffix
    suffix = bytearray()
    if sample_type & PERF_SAMPLE_TID:
        suffix += struct.pack('<II', ppid, ptid)
    if sample_type & PERF_SAMPLE_TIME:
        suffix += struct.pack('<Q', ts_ns)
    if sample_type & PERF_SAMPLE_ID:
        suffix += struct.pack('<Q', sample_id)
    if sample_type & PERF_SAMPLE_CPU:
        suffix += struct.pack('<II', 0, 0)
    payload += suffix
    payload = _pad_to(payload, 8)

    header = struct.pack('<IHH', PERF_RECORD_FORK, 0, 8 + len(payload))
    return bytes(header) + bytes(payload)


def _sched_switch_raw(prev_comm: str, prev_pid: int, prev_prio: int,
                      prev_state: int, next_comm: str, next_pid: int,
                      next_prio: int) -> bytes:
    """Build sched_switch tracepoint raw data (with 8-byte common fields header)."""
    raw = bytearray()
    # Common fields: type(2) + flags(1) + preempt_count(1) + pid(4) = 8 bytes
    raw += struct.pack('<HBBi', 0, 0, 0, prev_pid)
    # prev_comm: char[16]
    pc = prev_comm.encode("utf-8")[:15] + b'\x00'
    raw += pc + b'\x00' * (16 - len(pc))
    # prev_pid: i32
    raw += struct.pack('<i', prev_pid)
    # prev_prio: i32
    raw += struct.pack('<i', prev_prio)
    # prev_state: i64 (kernel 6.x)
    raw += struct.pack('<q', prev_state)
    # next_comm: char[16]
    nc = next_comm.encode("utf-8")[:15] + b'\x00'
    raw += nc + b'\x00' * (16 - len(nc))
    # next_pid: i32
    raw += struct.pack('<i', next_pid)
    # next_prio: i32
    raw += struct.pack('<i', next_prio)
    return bytes(raw)  # 64 bytes total


def _empty_raw() -> bytes:
    """Empty raw data for probes that carry no tracepoint fields."""
    # 8-byte common header only (type(2)+flags(1)+preempt_count(1)+pid(4))
    return struct.pack('<HBBi', 0, 0, 0, 0)


def generate_perf_data(output_dir: str) -> str:
    """Generate a synthetic perf.data with sched, GIL, and GPU events."""
    perf_path = os.path.join(output_dir, "perf.data")

    # sample_type: TID + TIME + ID + CPU + RAW
    sample_type = (PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID |
                   PERF_SAMPLE_CPU | PERF_SAMPLE_RAW)

    # Define event types: (name, tracepoint_id, sample_id_base)
    # Each event type gets its own attr with unique tracepoint ID.
    # We assign sample IDs so the reader can map samples back to attrs.
    EVENT_TYPES = [
        ("sched:sched_switch",       100, 1000),
        ("python:take_gil",          101, 2000),
        ("python:take_gil_return",   102, 3000),
        ("python:drop_gil",          103, 4000),
        ("nvidia:launch",            104, 5000),
        ("nvidia:launch_ret",        105, 6000),
        ("nvidia:stream_sync",       106, 7000),
        ("nvidia:stream_sync_ret",   107, 8000),
    ]
    num_event_types = len(EVENT_TYPES)

    # Build attrs and ID sections
    # Each attr entry in the attrs section: attr(ATTR_SIZE) + FileSection(16)
    # The FileSection points to an array of uint64 IDs for that attr.
    attrs = []
    for name, tp_id, sid_base in EVENT_TYPES:
        attr = _make_attr(tp_id, sample_type)
        attrs.append((attr, sid_base))

    # We'll place IDs right after the attrs section
    # IDs layout: each attr gets 1 ID (its sid_base)
    ids_per_attr = 1

    # ---- Generate events ----
    events_data = bytearray()
    base_ts = BASE_TS_NS

    # COMM events for each thread
    for tid, name in [(TID_MAIN, "python3"), (TID_MCPROC0, "python3"),
                      (TID_MCPROC1, "python3")]:
        events_data += _make_comm(PID, tid, name, base_ts, sample_type,
                                  EVENT_TYPES[0][2])  # use sched sample_id

    # FORK events to establish TID->TGID mapping
    for tid in [TID_MCPROC0, TID_MCPROC1]:
        events_data += _make_fork(PID, PID, tid, TID_MAIN, base_ts,
                                  sample_type, EVENT_TYPES[0][2])

    # sched_switch events: threads going on/off CPU every ~200ms
    sched_events = []
    for i in range(75):  # 75 switches across 15 seconds
        ts = base_ts + i * 200_000_000  # 200ms apart
        if ts >= base_ts + DURATION_S * 1_000_000_000:
            break
        tid = [TID_MAIN, TID_MCPROC0, TID_MCPROC1][i % 3]
        other_tid = [TID_MCPROC0, TID_MCPROC1, TID_MAIN][i % 3]
        # Switch out tid, switch in other_tid
        raw = _sched_switch_raw(
            "python3", tid, 120, 1,  # prev: sleeping
            "python3", other_tid, 120)
        sched_events.append((ts, tid, raw, EVENT_TYPES[0][2]))

    # GIL events: take_gil -> take_gil_return -> ... -> drop_gil
    # Pattern across threads, ~every 300ms
    gil_events = []
    for i in range(50):
        ts = base_ts + i * 300_000_000  # 300ms apart
        if ts >= base_ts + DURATION_S * 1_000_000_000:
            break
        tid = [TID_MAIN, TID_MCPROC0, TID_MCPROC1][i % 3]
        raw = _empty_raw()
        # take_gil
        gil_events.append((ts, tid, raw, EVENT_TYPES[1][2]))                # take_gil
        # take_gil_return (50us later - GIL acquired)
        gil_events.append((ts + 50_000, tid, raw, EVENT_TYPES[2][2]))       # take_gil_return
        # drop_gil (5ms later)
        gil_events.append((ts + 5_000_000, tid, raw, EVENT_TYPES[3][2]))    # drop_gil

    # GPU events: nvidia:launch/ret and nvidia:stream_sync/ret
    gpu_events = []
    for i in range(30):
        ts = base_ts + i * 500_000_000  # 500ms apart
        if ts >= base_ts + DURATION_S * 1_000_000_000:
            break
        tid = [TID_MAIN, TID_MCPROC0][i % 2]
        raw = _empty_raw()
        # launch + launch_ret (200us kernel launch)
        gpu_events.append((ts, tid, raw, EVENT_TYPES[4][2]))                # nvidia:launch
        gpu_events.append((ts + 200_000, tid, raw, EVENT_TYPES[5][2]))      # nvidia:launch_ret
        # stream_sync + stream_sync_ret (2ms later, 1ms sync)
        sync_ts = ts + 2_000_000
        gpu_events.append((sync_ts, tid, raw, EVENT_TYPES[6][2]))           # nvidia:stream_sync
        gpu_events.append((sync_ts + 1_000_000, tid, raw, EVENT_TYPES[7][2]))  # nvidia:stream_sync_ret

    # Sort all sample events by timestamp
    all_samples = []
    for ts, tid, raw, sid in sched_events:
        all_samples.append((ts, PID, tid, raw, sid))
    for ts, tid, raw, sid in gil_events:
        all_samples.append((ts, PID, tid, raw, sid))
    for ts, tid, raw, sid in gpu_events:
        all_samples.append((ts, PID, tid, raw, sid))

    all_samples.sort(key=lambda x: x[0])

    # Map sample_id -> attr index (for find_attr_index)
    sid_to_attr = {}
    for idx, (name, tp_id, sid_base) in enumerate(EVENT_TYPES):
        sid_to_attr[sid_base] = idx

    for ts, pid, tid, raw, sid in all_samples:
        events_data += _make_sample(pid, tid, ts, ts % 4, raw, sid,
                                    sample_type)

    # ---- Build file layout ----
    # Layout: header(104) | attrs_with_ids | ids_data | data | feature_sections | event_desc

    # Attrs section: N entries of (attr + FileSection)
    entry_size = ATTR_SIZE + 16  # attr + FileSection for IDs

    attrs_offset = HEADER_SIZE
    # IDs data comes right after attrs section
    ids_data_offset = attrs_offset + num_event_types * entry_size
    ids_data = bytearray()
    attrs_entries = bytearray()

    for i, (attr, sid_base) in enumerate(attrs):
        attrs_entries += attr
        # FileSection pointing to this attr's IDs
        id_offset = ids_data_offset + len(ids_data)
        id_size = ids_per_attr * 8  # 1 ID, 8 bytes each
        attrs_entries += struct.pack('<QQ', id_offset, id_size)
        # Write the ID value
        ids_data += struct.pack('<Q', sid_base)

    attrs_size = len(attrs_entries)

    data_offset = ids_data_offset + len(ids_data)
    data_size = len(events_data)

    # Feature sections come after data
    feature_sections_offset = data_offset + data_size
    # One FileSection (16 bytes) for EVENT_DESC
    event_desc_data_offset = feature_sections_offset + 16

    # Build EVENT_DESC section
    event_desc = bytearray()
    event_desc += struct.pack('<II', num_event_types, ATTR_SIZE)

    for i, (attr, sid_base) in enumerate(attrs):
        name = EVENT_TYPES[i][0]
        event_desc += attr  # perf_event_attr

        # nr_ids
        event_desc += struct.pack('<I', ids_per_attr)

        # Event name: null-terminated, padded to 4-byte alignment
        name_bytes = name.encode("utf-8") + b'\x00'
        while len(name_bytes) % 4 != 0:
            name_bytes += b'\x00'
        event_desc += struct.pack('<I', len(name_bytes))
        event_desc += name_bytes

        # IDs
        event_desc += struct.pack('<Q', sid_base)

    feature_section = struct.pack('<QQ', event_desc_data_offset, len(event_desc))

    # flags: bit 12 = EVENT_DESC
    flags = [1 << FEAT_EVENT_DESC, 0, 0, 0]

    # Build file header
    file_header = struct.pack('<QQQ',
                              PERF_MAGIC,
                              HEADER_SIZE,
                              ATTR_SIZE)
    # attrs section: offset + size
    file_header += struct.pack('<QQ', attrs_offset, attrs_size)
    # data section: offset + size
    file_header += struct.pack('<QQ', data_offset, data_size)
    # event_types section (unused)
    file_header += struct.pack('<QQ', 0, 0)
    # flags
    file_header += struct.pack('<QQQQ', *flags)

    assert len(file_header) == HEADER_SIZE

    with open(perf_path, 'wb') as f:
        f.write(file_header)
        f.write(bytes(attrs_entries))
        f.write(bytes(ids_data))
        f.write(bytes(events_data))
        f.write(feature_section)
        f.write(bytes(event_desc))

    total_samples = len(all_samples)
    print(f"Synthetic perf.data written to {perf_path} "
          f"({total_samples} samples, {num_event_types} event types)")
    return perf_path


# =========================================================================
# Main
# =========================================================================

def generate_all(output_dir: str) -> Tuple[str, str]:
    """Generate both ftrc and perf.data files. Returns (ftrc_path, perf_path)."""
    os.makedirs(output_dir, exist_ok=True)
    ftrc_path = generate_ftrc(output_dir)
    perf_path = generate_perf_data(output_dir)
    return ftrc_path, perf_path


def main():
    parser = argparse.ArgumentParser(
        description="Generate synthetic .ftrc and perf.data for perf-viz-merge testing")
    parser.add_argument("--output-dir", default=".",
                        help="Output directory (default: current directory)")
    args = parser.parse_args()
    generate_all(args.output_dir)


if __name__ == "__main__":
    main()
