// NativeXRenderer - the machine it runs on, for the log (D-next).
//
// A player's log should say what it was measured on: the processor (its name, cores and logical processors, and on a
// processor with two kinds of core how many of each), the logical processors the process may run on (a measurement
// that holds the game to a few of them shows), and the memory. The processor time of the process and of a thread, read
// once per log line, give the cores the game used.

#pragma once

#include <cstdint>
#include <string>

namespace native_x_renderer::machine {

// "<brand>, N cores (P of the faster kind, E of the other) and L logical processors; the process on A of them (mask);
// R GB of memory".
std::string Describe();
// The process's processor time so far, and the calling thread's, in time stamp counter ticks (QueryProcessCycleTime,
// QueryThreadCycleTime: invariant TSC ticks while the threads ran); 0 where unknown.
uint64_t ProcessCycles();
uint64_t ThreadCycles();
// The logical processors the process may run on now, and its affinity mask (the first processor group's).
uint32_t Affinity(uint64_t& mask);

}  // namespace native_x_renderer::machine
