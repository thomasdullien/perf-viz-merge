# Performance Optimization 3+4: Combined optimizations

Baseline: 5.4s -> After all optimizations: 2.93s (46% faster, 1.84x speedup)

Key changes: string_view VizEvent fields, -O3, reusable ProtoEncoder,
write_packet optimization, 1MB write buffer, write_nested backpatching.
