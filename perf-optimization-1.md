# Performance Optimization 1: Reserve Vector Capacity

## What profiling showed

Using `perf record -g --call-graph dwarf`, the top hotspots in the baseline were:

| % Time | Function | Category |
|--------|----------|----------|
| 14.12% | std::__introsort_loop (VizEvent sort) | Sort |
| 6.70%  | main | Reading/processing |
| 5.44%  | ProtoEncoder::write_varint | Protobuf encoding |
| 5.35%  | vector::_M_realloc_insert (VizEvent push_back) | Vector realloc |
| 5.32%  | __memmove_evex_unaligned_erms | Memory copies from realloc |
| 2.49%  | ftrc_next | File reading |

The vector reallocation (5.35%) and associated memmove (5.32%) together consumed ~10.7% of runtime. This happens because the `viz_events` vector starts empty and grows geometrically as ~5M events are pushed back, causing repeated full-vector copies of objects containing `std::string` members.

## What was changed

Added a pre-estimation step that uses `stat()` to get file sizes before reading, then calls `reserve()` with an estimated event count (file_size / 24, based on 12-byte binary events and ~2 raw events per completed event).

This eliminates all intermediate reallocations and the associated memmove calls.

## Before/after timing

**Before (baseline):**
- Wall: 5.3-5.5s
- User: 4.04-4.21s
- Sys: 1.17-1.31s

**After:**
- Wall: 4.73-4.79s
- User: 3.84-3.90s
- Sys: 0.70s

**Improvement: ~12% wall-clock speedup** (0.6-0.7s saved)

The sys time dropped dramatically (1.2s -> 0.7s) because the kernel no longer needs to service mmap/brk syscalls for repeated vector growth. User time also dropped because there are no more element-by-element copy constructions of VizEvent (which includes std::string deep copies).
