// Diagnostic tool: dump all perf events for a specific thread, in order,
// to understand the actual sched_switch / sched_stat_runtime / context-switch
// / GIL event sequence.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "event_types.h"
#include "perf_data_reader.h"

static const char *type_name(PerfEventType t) {
    switch (t) {
    case PerfEventType::SchedSwitch:       return "SchedSwitch";
    case PerfEventType::SchedWakeup:       return "SchedWakeup";
    case PerfEventType::SchedFork:         return "SchedFork";
    case PerfEventType::SchedStatRuntime:  return "SchedStatRuntime";
    case PerfEventType::ContextSwitch:     return "ContextSwitch";
    case PerfEventType::TakeGil:           return "TakeGil";
    case PerfEventType::TakeGilReturn:     return "TakeGilReturn";
    case PerfEventType::DropGil:           return "DropGil";
    case PerfEventType::NvidiaLaunch:      return "NvidiaLaunch";
    case PerfEventType::NvidiaLaunchReturn:return "NvidiaLaunchRet";
    default:                               return "Other";
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fmt::print(stderr, "Usage: {} <perf.data> <tid> [max_events]\n", argv[0]);
        return 1;
    }

    std::string perf_path = argv[1];
    int32_t target_tid = std::atoi(argv[2]);
    int max_events = argc > 3 ? std::atoi(argv[3]) : 500;

    std::vector<PerfEvent> events;

    PerfDataReader reader(perf_path);
    reader.read_all_events([&](const PerfEvent &event) {
        // Collect events where the thread is involved
        bool dominated = false;
        if (event.tid == target_tid) dominated = true;
        if (event.type == PerfEventType::SchedSwitch) {
            if (event.data.sched_switch.prev_tid == target_tid ||
                event.data.sched_switch.next_tid == target_tid)
                dominated = true;
        }
        if (event.type == PerfEventType::SchedWakeup) {
            if (event.data.wakeup.target_tid == target_tid)
                dominated = true;
        }
        if (dominated) {
            events.push_back(event);
        }
    });

    std::sort(events.begin(), events.end(),
              [](const PerfEvent &a, const PerfEvent &b) {
                  return a.timestamp_ns < b.timestamp_ns;
              });

    fmt::print("Found {} events for tid {}\n\n", events.size(), target_tid);

    int printed = 0;
    for (const auto &e : events) {
        if (printed >= max_events) {
            fmt::print("... truncated at {} events\n", max_events);
            break;
        }

        double ts_ms = static_cast<double>(e.timestamp_ns) / 1e6;  // ms for readability

        if (e.type == PerfEventType::SchedSwitch) {
            fmt::print("{:>14.3f}ms  {:20s}  tid={:<6d} cpu={:<2d}  "
                       "prev={}({}) state={} -> next={}({})\n",
                       ts_ms, type_name(e.type), e.tid, e.cpu,
                       e.data.sched_switch.prev_tid,
                       e.data.sched_switch.prev_comm,
                       e.data.sched_switch.prev_state,
                       e.data.sched_switch.next_tid,
                       e.data.sched_switch.next_comm);
        } else if (e.type == PerfEventType::SchedWakeup) {
            fmt::print("{:>14.3f}ms  {:20s}  tid={:<6d} cpu={:<2d}  "
                       "target={}\n",
                       ts_ms, type_name(e.type), e.tid, e.cpu,
                       e.data.wakeup.target_tid);
        } else {
            fmt::print("{:>14.3f}ms  {:20s}  tid={:<6d} cpu={:<2d}\n",
                       ts_ms, type_name(e.type), e.tid, e.cpu);
        }
        printed++;
    }

    return 0;
}
