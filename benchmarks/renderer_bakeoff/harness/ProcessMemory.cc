#include "harness/ProcessMemory.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <cinttypes>
#include <cstdio>
#endif

namespace donner::bakeoff {

#if defined(__APPLE__)

MemorySample SampleProcessMemory() {
  MemorySample sample;
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS) {
    return sample;
  }
  sample.residentBytes = static_cast<std::int64_t>(info.resident_size);
  if (count >= TASK_VM_INFO_REV1_COUNT) {
    sample.footprintBytes = static_cast<std::int64_t>(info.phys_footprint);
  }
  // The footprint peak and the tagged graphics ledger arrived together in revision 3.
  if (count >= TASK_VM_INFO_REV3_COUNT) {
    sample.peakBytes = info.ledger_phys_footprint_peak;
    sample.graphicsBytes = info.ledger_tag_graphics_footprint;
  }
  return sample;
}

#elif defined(__linux__)

MemorySample SampleProcessMemory() {
  MemorySample sample;
  std::FILE* status = std::fopen("/proc/self/status", "r");
  if (status == nullptr) {
    return sample;
  }
  char line[256];
  while (std::fgets(line, sizeof(line), status) != nullptr) {
    std::int64_t kilobytes = 0;
    if (std::sscanf(line, "VmRSS: %" SCNd64 " kB", &kilobytes) == 1) {
      sample.residentBytes = kilobytes * 1024;
    } else if (std::sscanf(line, "VmHWM: %" SCNd64 " kB", &kilobytes) == 1) {
      sample.peakBytes = kilobytes * 1024;
    }
  }
  std::fclose(status);
  return sample;
}

#else

MemorySample SampleProcessMemory() {
  return MemorySample{};
}

#endif

}  // namespace donner::bakeoff
