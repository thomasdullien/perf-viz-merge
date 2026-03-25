# Performance Optimization 2: Buffered I/O and ProtoEncoder improvements

## What profiling showed

After optimization 1, the updated profile showed:

| % Time | Function |
|--------|----------|
| 16.85% | std::sort (VizEvent) |
| 7.85%  | main |
| 6.65%  | ProtoEncoder::write_varint |
| 6.43%  | memmove |
| 2.96%  | _int_free |
| 2.47%  | Hash_bytes (string hashing) |
| 2.09%  | malloc |
| 1.83%  | fwrite |

Key observations:
- ProtoEncoder::write_varint at 6.65% - encoding overhead
- write_nested creates new ProtoEncoder objects (heap alloc) per nested message
- fwrite at 1.83% - many small writes to disk
- malloc/free ~5% combined from temporary string/encoder allocations

## What was changed

1. **1MB output buffer**: Added `setvbuf()` with a 1MB buffer to reduce fwrite syscall frequency.
2. **ProtoEncoder::write_nested rewrite**: Instead of creating a sub-encoder, writing into it, then copying the result, the new implementation writes directly into the parent buffer using a backpatch approach: write a length placeholder, emit content, then fix up the length varint in-place.
3. **ProtoEncoder::write_varint fast path**: Added single-byte fast path for values <= 0x7f (most field tags and small values).
4. **Reserve 256 bytes**: ProtoEncoder buffers pre-reserve 256 bytes to avoid initial growth.

## Before/after timing

**Before (after opt 1):**
- Wall: 4.73-4.79s
- User: 3.84-3.90s

**After:**
- Wall: 4.61-4.67s
- User: 3.72-3.81s

**Improvement: ~3% wall-clock speedup** (0.1-0.15s saved)

The improvement is moderate because the write path is only ~15% of total time. The sort remains the dominant hotspot at ~17%.
