#pragma once

#include <cstdint>
#include <cmath>

// Aligns timestamps between perf (nanoseconds, CLOCK_MONOTONIC) and
// VizTracer (microseconds, typically CLOCK_MONOTONIC via Python's time.monotonic()).
//
// When both use CLOCK_MONOTONIC, only unit conversion is needed.
// When clocks differ, an offset can be auto-detected or manually specified.

class ClockAligner {
public:
    // Set a manual offset (in microseconds) to be added to VizTracer timestamps.
    void set_manual_offset(double offset_us) {
        offset_us_ = offset_us;
        manual_ = true;
    }

    // Auto-detect the offset from the timestamp ranges of both sources.
    // Call this after a first pass over both data sources.
    void detect(double perf_first_ns, double perf_last_ns,
                double viz_first_us, double viz_last_us) {
        if (manual_) return;

        double perf_first_us = perf_first_ns / 1000.0;
        double perf_last_us = perf_last_ns / 1000.0;

        // Detect whether both sources use the same clock.
        //
        // Case 1: Both use CLOCK_MONOTONIC — timestamps are large
        // absolute values (system uptime) and their ranges overlap or
        // are close.  No offset needed.
        //
        // Case 2: VizTracer uses epoch-relative or zero-based timestamps
        // while perf uses CLOCK_MONOTONIC.  Need to align start times.
        //
        // Heuristic: if both first-timestamps are in the same order of
        // magnitude (both > 1e9 μs = ~17 minutes uptime, which is always
        // true for CLOCK_MONOTONIC) AND the difference is less than the
        // combined recording duration, they share the same clock.
        double diff = std::abs(perf_first_us - viz_first_us);
        double perf_range = perf_last_us - perf_first_us;
        double viz_range = viz_last_us - viz_first_us;
        double combined_range = perf_range + viz_range;

        bool both_large = perf_first_us > 1e9 && viz_first_us > 1e9;
        bool overlapping = diff < combined_range;

        if (both_large && overlapping) {
            // Same clock (CLOCK_MONOTONIC) — recordings may have started
            // at different times but timestamps are already comparable.
            offset_us_ = 0.0;
        } else {
            // Different clock bases — align start times
            offset_us_ = perf_first_us - viz_first_us;
        }
    }

    // Convert a perf timestamp (ns) to output microseconds.
    double align_perf(uint64_t perf_ts_ns) const {
        return static_cast<double>(perf_ts_ns) / 1000.0;
    }

    // Convert a VizTracer timestamp (us) to output microseconds.
    double align_viz(double viz_ts_us) const {
        return viz_ts_us + offset_us_;
    }

    double offset_us() const { return offset_us_; }

private:
    double offset_us_ = 0.0;
    bool manual_ = false;
};
