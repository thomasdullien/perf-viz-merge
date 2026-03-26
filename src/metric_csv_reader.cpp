#include "metric_csv_reader.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

#include <fmt/format.h>

MetricCsvReader::MetricCsvReader(const std::string &path) : path_(path) {}

std::vector<MetricSample> MetricCsvReader::read_all() const {
    return read_all("");
}

std::vector<MetricSample> MetricCsvReader::read_all(const std::string &prefix) const {
    std::ifstream file(path_);
    if (!file.is_open()) {
        throw std::runtime_error(
            fmt::format("Cannot open metrics CSV: {}", path_));
    }

    // Parse header line to get column names
    std::string header_line;
    if (!std::getline(file, header_line)) {
        throw std::runtime_error(
            fmt::format("Empty metrics CSV: {}", path_));
    }

    std::vector<std::string> columns;
    {
        std::istringstream ss(header_line);
        std::string col;
        while (std::getline(ss, col, ',')) {
            // Trim whitespace
            while (!col.empty() && (col.front() == ' ' || col.front() == '\t'))
                col.erase(col.begin());
            while (!col.empty() && (col.back() == ' ' || col.back() == '\t' ||
                                     col.back() == '\r' || col.back() == '\n'))
                col.pop_back();
            columns.push_back(col);
        }
    }

    if (columns.empty() || columns[0] != "timestamp_ns") {
        throw std::runtime_error(fmt::format(
            "Metrics CSV must have 'timestamp_ns' as first column, got '{}' in {}",
            columns.empty() ? "(empty)" : columns[0], path_));
    }

    // Build metric names for each non-timestamp column
    std::vector<std::string> metric_names;
    for (size_t i = 1; i < columns.size(); i++) {
        if (prefix.empty()) {
            metric_names.push_back(columns[i]);
        } else {
            metric_names.push_back(prefix + columns[i]);
        }
    }

    // Parse data rows
    std::vector<MetricSample> samples;
    std::string line;
    int line_num = 1;

    while (std::getline(file, line)) {
        line_num++;
        if (line.empty() || line[0] == '#') continue;

        // Remove trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::istringstream ss(line);
        std::string field;

        // Parse timestamp
        if (!std::getline(ss, field, ',')) continue;
        char *end;
        uint64_t ts_ns = std::strtoull(field.c_str(), &end, 10);
        if (end == field.c_str()) continue;  // skip unparseable lines

        double ts_us = static_cast<double>(ts_ns) / 1000.0;

        // Parse each metric column
        for (size_t i = 0; i < metric_names.size(); i++) {
            if (!std::getline(ss, field, ',')) break;

            // Trim whitespace
            while (!field.empty() && field.front() == ' ') field.erase(field.begin());

            // Skip non-numeric values (e.g. "-" from nvidia-smi)
            if (field.empty() || field == "-" || field == "N/A") continue;

            double value = std::strtod(field.c_str(), &end);
            if (end == field.c_str()) continue;  // skip unparseable

            MetricSample s;
            s.ts_us = ts_us;
            s.value = value;
            s.name = metric_names[i];
            samples.push_back(std::move(s));
        }
    }

    // Sort by timestamp (stable sort preserves column order within same ts)
    std::stable_sort(samples.begin(), samples.end(),
                     [](const MetricSample &a, const MetricSample &b) {
                         return a.ts_us < b.ts_us;
                     });

    return samples;
}
