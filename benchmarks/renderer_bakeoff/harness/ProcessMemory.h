#pragma once
/// @file
/// Process memory sampling for the benchmark. Each benchmark run is its own process, so process
/// peaks belong to exactly one backend and scene.

#include <cstdint>

namespace donner::bakeoff {

/// One process memory sample. Fields the platform does not report are -1.
struct MemorySample {
  /// Resident set size in bytes.
  std::int64_t residentBytes = -1;
  /// macOS physical footprint (the figure Activity Monitor reports) in bytes. On unified-memory
  /// Macs it includes GPU allocations attributed to the process.
  std::int64_t footprintBytes = -1;
  /// Peak of \ref footprintBytes (macOS) or peak resident set size (Linux, VmHWM) in bytes.
  std::int64_t peakBytes = -1;
  /// macOS graphics-tagged footprint in bytes: memory the GPU driver attributes to the process.
  std::int64_t graphicsBytes = -1;
};

/// Samples the current process.
MemorySample SampleProcessMemory();

}  // namespace donner::bakeoff
