// Diagnostic: show which threads have GPU events and whether they're in comm_map
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>
#include "event_types.h"
#include "perf_data_reader.h"

static bool is_gpu_event(PerfEventType t) {
    switch (t) {
    case PerfEventType::NvidiaLaunch:
    case PerfEventType::NvidiaLaunchReturn:
    case PerfEventType::NvidiaStreamSync:
    case PerfEventType::NvidiaStreamSyncReturn:
    case PerfEventType::NvidiaDeviceSync:
    case PerfEventType::NvidiaDeviceSyncReturn:
    case PerfEventType::NvidiaEventSync:
    case PerfEventType::NvidiaEventSyncReturn:
    case PerfEventType::NvidiaMemcpyHtoD:
    case PerfEventType::NvidiaMemcpyHtoDReturn:
    case PerfEventType::NvidiaMemcpyDtoH:
    case PerfEventType::NvidiaMemcpyDtoHReturn:
    case PerfEventType::NvidiaMemcpyDtoD:
    case PerfEventType::NvidiaMemcpyDtoDReturn:
    case PerfEventType::NvidiaMemcpyAsync:
    case PerfEventType::NvidiaMemcpyAsyncReturn:
    case PerfEventType::NvidiaMemcpyPeer:
    case PerfEventType::NvidiaMemcpyPeerReturn:
    case PerfEventType::NvidiaMalloc:
    case PerfEventType::NvidiaMallocReturn:
    case PerfEventType::NvidiaFree:
    case PerfEventType::NvidiaFreeReturn:
    case PerfEventType::NcclAllReduce:
    case PerfEventType::NcclAllReduceReturn:
    case PerfEventType::NcclBroadcast:
    case PerfEventType::NcclBroadcastReturn:
    case PerfEventType::NcclReduceScatter:
    case PerfEventType::NcclReduceScatterReturn:
        return true;
    default:
        return false;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fmt::print(stderr, "Usage: {} <perf.data>\n", argv[0]);
        return 1;
    }

    PerfDataReader reader(argv[1]);

    // tid -> count of GPU events
    std::map<int32_t, int> gpu_by_tid;
    // tid -> pid from event header
    std::map<int32_t, int32_t> tid_to_pid;

    reader.read_all_events([&](const PerfEvent &event) {
        if (is_gpu_event(event.type)) {
            gpu_by_tid[event.tid]++;
            tid_to_pid[event.tid] = event.pid;
        }
    });

    auto comm = reader.comm_map();

    fmt::print("GPU events by thread:\n");
    for (auto &[tid, count] : gpu_by_tid) {
        auto it = comm.find(tid);
        std::string name = it != comm.end() ? it->second : "<not in comm_map>";
        fmt::print("  tid={:<8d}  pid={:<8d}  count={:<6d}  comm={}\n",
                   tid, tid_to_pid[tid], count, name);
    }

    return 0;
}
