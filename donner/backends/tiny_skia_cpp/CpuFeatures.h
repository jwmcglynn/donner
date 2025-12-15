/// @file
/// Runtime CPU feature detection helpers for tiny_skia_cpp SIMD dispatch.

#pragma once

#include <cstdint>

namespace donner::backends::tiny_skia_cpp {

/** Captures a subset of CPU SIMD capabilities used by tiny_skia_cpp. */
struct CpuFeatures {
  bool hasSse2{false};
  bool hasAvx2{false};
  bool hasNeon{false};
};

/** Return the process-wide CPU feature flags, computed once on first use. */
const CpuFeatures& GetCpuFeatures();

}  // namespace donner::backends::tiny_skia_cpp
