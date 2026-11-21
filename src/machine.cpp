// NativeXRenderer - see machine.h.

#include "machine.h"

#include <fmt/format.h>

#include <bitset>
#include <functional>
#include <cstring>
#include <map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#endif

namespace native_x_renderer::machine {

namespace {

uint32_t Bits(uint64_t mask) { return uint32_t(std::bitset<64>(mask).count()); }

}  // namespace

std::string Describe() {
#ifdef _WIN32
  // The brand string (CPUID 0x80000002-4), without its padding.
  char brand[49] = {};
  int regs[4] = {};
  __cpuid(regs, int(0x80000000u));
  if (uint32_t(regs[0]) >= 0x80000004u) {
    for (int i = 0; i < 3; ++i) {
      __cpuid(regs, int(0x80000002u + i));
      std::memcpy(brand + 16 * i, regs, 16);
    }
  }
  std::string name(brand);
  name.erase(0, name.find_first_not_of(' '));
  while (!name.empty() && name.back() == ' ') name.pop_back();
  if (name.empty()) name = "an unknown processor";

  // Cores and their logical processors, by efficiency class (a hybrid processor's performance cores have the higher).
  uint32_t cores = 0, logical = 0;
  std::map<int, std::pair<uint32_t, uint32_t>, std::greater<int>> classes;  // class -> cores, logical processors
  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  std::vector<uint8_t> buffer(length);
  if (length && GetLogicalProcessorInformationEx(
                    RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                    &length)) {
    for (DWORD offset = 0; offset < length;) {
      const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
      uint32_t threads = 0;
      for (WORD g = 0; g < info->Processor.GroupCount; ++g) threads += Bits(uint64_t(info->Processor.GroupMask[g].Mask));
      ++cores;
      logical += threads;
      auto& c = classes[info->Processor.EfficiencyClass];
      ++c.first;
      c.second += threads;
      offset += info->Size;
    }
  }
  std::string kinds;
  if (classes.size() > 1) {
    kinds = " (";
    bool first = true;
    for (const auto& [efficiency, counts] : classes) {
      kinds += fmt::format("{}{} of efficiency class {} with {} logical processors", first ? "" : ", ", counts.first,
                           efficiency, counts.second);
      first = false;
    }
    kinds += ")";
  }
  uint64_t mask = 0;
  const uint32_t allowed = Affinity(mask);
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  GlobalMemoryStatusEx(&memory);
  const int parked = Parked(mask);
  return fmt::format("{}, {} cores{} and {} logical processors; the process on {} of them (mask {:#x}, {} parked now); "
                     "{:.1f} GB of memory",
                     name, cores, kinds, logical, allowed, mask, parked < 0 ? std::string("unknown how many") :
                     std::to_string(parked), double(memory.ullTotalPhys) / double(1ull << 30));
#else
  return "an unknown machine";
#endif
}

uint64_t ProcessCycles() {
#ifdef _WIN32
  ULONG64 cycles = 0;
  if (QueryProcessCycleTime(GetCurrentProcess(), &cycles)) return uint64_t(cycles);
#endif
  return 0;
}

uint64_t ThreadCycles() {
#ifdef _WIN32
  ULONG64 cycles = 0;
  if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) return uint64_t(cycles);
#endif
  return 0;
}

uint32_t Affinity(uint64_t& mask) {
  mask = 0;
#ifdef _WIN32
  DWORD_PTR process = 0, system = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) mask = uint64_t(process);
#endif
  return Bits(mask);
}

int Parked(uint64_t mask) {
#ifdef _WIN32
  ULONG length = 0;
  GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
  if (!length) return -1;
  std::vector<uint8_t> buffer(length);
  if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length, &length,
                                  GetCurrentProcess(), 0)) {
    return -1;
  }
  int parked = 0;
  for (ULONG offset = 0; offset + sizeof(DWORD) * 2 <= length;) {
    const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
    if (!info->Size) break;
    if (info->Type == CpuSetInformation && info->CpuSet.Group == 0 && info->CpuSet.LogicalProcessorIndex < 64 &&
        (mask >> info->CpuSet.LogicalProcessorIndex) & 1 && info->CpuSet.Parked) {
      ++parked;
    }
    offset += info->Size;
  }
  return parked;
#else
  return -1;
#endif
}

}  // namespace native_x_renderer::machine
