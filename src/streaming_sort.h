#pragma once

#include <algorithm>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

#include "event_types.h"
#include "perf_data_reader.h"

// Abstract iterator for consuming perf events in sorted timestamp order.
class PerfEventIterator {
public:
    virtual ~PerfEventIterator() = default;
    virtual bool has_next() const = 0;
    virtual const PerfEvent &peek() const = 0;
    virtual void advance() = 0;
};

// Comparator: min-heap by timestamp, with SchedSwitch sorting last at equal
// timestamps (matches the sort order in merge_engine.cpp).
struct PerfEventGreater {
    bool operator()(const PerfEvent &a, const PerfEvent &b) const {
        if (a.timestamp_ns != b.timestamp_ns)
            return a.timestamp_ns > b.timestamp_ns;
        auto pri = [](PerfEventType t) -> int {
            return t == PerfEventType::SchedSwitch ? 1 : 0;
        };
        return pri(a.type) > pri(b.type);
    }
};

// Wraps a pre-sorted vector (small-file fast path).
class VectorPerfIterator : public PerfEventIterator {
public:
    explicit VectorPerfIterator(std::vector<PerfEvent> events)
        : events_(std::move(events)), pos_(0) {}

    bool has_next() const override { return pos_ < events_.size(); }
    const PerfEvent &peek() const override { return events_[pos_]; }
    void advance() override { ++pos_; }

private:
    std::vector<PerfEvent> events_;
    size_t pos_;
};

// Streaming reorder buffer for large perf.data files.
//
// perf.data is almost sorted (events are sorted within each CPU ring buffer,
// but interleaved across CPUs). The max out-of-order distance is bounded by
// the perf buffer flush interval (typically 1-10ms).
//
// This iterator uses a bounded min-heap of `capacity` events. As new events
// are pulled from PerfDataReader, they are pushed into the heap. When the
// heap exceeds capacity, the minimum is popped — it is guaranteed globally
// correct as long as the out-of-order distance is less than the heap size.
//
// Memory: capacity * sizeof(PerfEvent) ≈ 1M * 88 = 88MB.
// No temp files. Single pass through perf.data.
class ReorderBufferIterator : public PerfEventIterator {
public:
    // reader:   perf data source (takes ownership)
    // capacity: max events in the reorder buffer (~1M = ~88MB)
    ReorderBufferIterator(std::unique_ptr<PerfDataReader> reader,
                          size_t capacity)
        : reader_(std::move(reader)), capacity_(capacity),
          reader_exhausted_(false), has_output_(false)
    {
        reader_->begin_iteration();
        // Fill the heap to capacity
        fill_heap();
        // Prepare the first output event
        pop_next();
    }

    bool has_next() const override { return has_output_; }

    const PerfEvent &peek() const override { return current_; }

    void advance() override { pop_next(); }

    // Metadata collected during iteration
    uint64_t min_timestamp_ns() const { return min_ts_; }
    uint64_t max_timestamp_ns() const { return max_ts_; }
    uint64_t total_events() const { return total_count_; }
    const std::vector<PerfEvent> &fork_events() const { return fork_events_; }

private:
    std::unique_ptr<PerfDataReader> reader_;
    size_t capacity_;
    bool reader_exhausted_;
    bool has_output_;
    PerfEvent current_;

    std::priority_queue<PerfEvent, std::vector<PerfEvent>, PerfEventGreater> heap_;

    // Metadata
    uint64_t min_ts_ = UINT64_MAX;
    uint64_t max_ts_ = 0;
    uint64_t total_count_ = 0;
    std::vector<PerfEvent> fork_events_;

    void track_metadata(const PerfEvent &e) {
        if (e.timestamp_ns < min_ts_) min_ts_ = e.timestamp_ns;
        if (e.timestamp_ns > max_ts_) max_ts_ = e.timestamp_ns;
        total_count_++;
        if (e.type == PerfEventType::SchedFork)
            fork_events_.push_back(e);
    }

    // Read events from the reader until the heap has `capacity_` events
    // or the reader is exhausted.
    void fill_heap() {
        PerfEvent e;
        while (heap_.size() < capacity_ && !reader_exhausted_) {
            if (reader_->next_event(e)) {
                track_metadata(e);
                heap_.push(e);
            } else {
                reader_exhausted_ = true;
            }
        }
    }

    // Pop the next sorted event. If the reader still has data, read one
    // event and push it into the heap first (maintaining the window size).
    void pop_next() {
        // Try to refill: read one event from reader, push into heap
        if (!reader_exhausted_) {
            PerfEvent e;
            if (reader_->next_event(e)) {
                track_metadata(e);
                heap_.push(e);
            } else {
                reader_exhausted_ = true;
            }
        }

        if (heap_.empty()) {
            has_output_ = false;
            return;
        }

        current_ = heap_.top();
        heap_.pop();
        has_output_ = true;
    }
};
