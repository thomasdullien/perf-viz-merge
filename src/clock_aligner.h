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

        // If both are using CLOCK_MONOTONIC, timestamps should be
        // in the same ballpark (within ~1 second of each other relative
        // to their ranges).
        double diff = std::abs(perf_first_us - viz_first_us);
        double range = std::max(perf_last_us - perf_first_us,
                                viz_last_us - viz_first_us);

        if (range > 0 && diff < range * 0.1) {
            // Timestamps are close enough — same clock, no offset needed
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
