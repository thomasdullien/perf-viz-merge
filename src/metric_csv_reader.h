#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A single metric sample (one value at one timestamp).
struct MetricSample {
    double ts_us;          // timestamp in microseconds (converted from ns)
    double value;          // metric value
    std::string name;      // metric name (e.g. "cpu_util_percent")
};

// Reads CSV files with CLOCK_MONOTONIC nanosecond timestamps and numeric
// metric columns. The first column must be "timestamp_ns". All other
// numeric columns become separate metric series.
//
// Example CSV:
//   timestamp_ns,cpu_util_percent,mem_util_percent
//   48312053179205000,42.5,65.2
//
class MetricCsvReader {
public:
    explicit MetricCsvReader(const std::string &path);

    // Read all samples from the CSV. Returns them sorted by timestamp,
    // with one MetricSample per (timestamp, column) pair.
    std::vector<MetricSample> read_all() const;

    // Read all samples, but prefix each metric name with the given string.
    // E.g. prefix="GPU " → "GPU gpu_util", "GPU mem_util"
    std::vector<MetricSample> read_all(const std::string &prefix) const;

private:
    std::string path_;
};
