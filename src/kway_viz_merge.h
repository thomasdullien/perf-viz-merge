#pragma once

#include <cstddef>
#include <memory>
#include <queue>
#include <vector>

#include "event_types.h"
#include "ftrc_reader.h"

// K-way merge iterator over multiple sorted FtrcReader streams.
// Produces a globally-sorted stream of VizEvents using a min-heap
// of size K (one entry per file). Memory: O(K), not O(N).

class KWayVizMerge : public VizEventIterator {
public:
    // Add a reader. Caller retains ownership (reader must outlive this).
    void add_source(FtrcReader *reader) {
        sources_.push_back(reader);
    }

    // Open all sources and seed the heap. Call after adding all sources.
    void initialize() {
        for (size_t i = 0; i < sources_.size(); i++) {
            sources_[i]->begin_iteration();
            if (sources_[i]->has_next()) {
                heap_.push({i});
            }
        }
        if (!heap_.empty()) {
            current_ = sources_[heap_.top().source_idx]->peek();
            has_current_ = true;
        }
    }

    bool has_next() const override { return has_current_; }

    const VizEvent &peek() const override { return current_; }

    void advance() override {
        if (heap_.empty()) {
            has_current_ = false;
            return;
        }

        // Pop the source that produced the current event
        HeapEntry top = heap_.top();
        heap_.pop();

        // Advance that source and re-insert if it has more
        sources_[top.source_idx]->advance();
        if (sources_[top.source_idx]->has_next()) {
            heap_.push(top);
        }

        // The next event is from whichever source is now at heap top
        if (!heap_.empty()) {
            current_ = sources_[heap_.top().source_idx]->peek();
            has_current_ = true;
        } else {
            has_current_ = false;
        }
    }

    uint64_t total_events() const {
        uint64_t total = 0;
        for (const auto *s : sources_)
            total += s->event_count();
        return total;
    }

private:
    struct HeapEntry {
        size_t source_idx;
    };

    // Min-heap comparator: same order as the old sort (ts, depth, tid, -dur).
    // Returns true if a should come AFTER b (std::priority_queue is max-heap
    // by default, so we invert for min-heap).
    struct HeapComparator {
        const KWayVizMerge *parent;
        bool operator()(const HeapEntry &a, const HeapEntry &b) const {
            const VizEvent &ea = parent->sources_[a.source_idx]->peek();
            const VizEvent &eb = parent->sources_[b.source_idx]->peek();
            if (ea.ts_us != eb.ts_us) return ea.ts_us > eb.ts_us;
            if (ea.depth >= 0 && eb.depth >= 0) {
                if (ea.depth != eb.depth) return ea.depth > eb.depth;
                if (ea.tid != eb.tid) return ea.tid > eb.tid;
            }
            return ea.dur_us < eb.dur_us;  // larger dur first
        }
    };

    std::vector<FtrcReader *> sources_;
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapComparator> heap_{
        HeapComparator{this}};
    VizEvent current_;
    bool has_current_ = false;
};
