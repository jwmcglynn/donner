#include "tiny_skia/wide/Wide.h"

#include "tiny_skia/wide/backend/ScalarMath.h"

namespace tiny_skia::wide {

float fasterMin(float lhs, float rhs) { return backend::scalar::fasterMin(lhs, rhs); }

float fasterMax(float lhs, float rhs) { return backend::scalar::fasterMax(lhs, rhs); }

SimdBuildMode configuredSimdBuildMode() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE)
  return SimdBuildMode::kNative;
#else
  return SimdBuildMode::kScalar;
#endif
}

const char* configuredSimdBuildModeName() {
  switch (configuredSimdBuildMode()) {
    case SimdBuildMode::kNative:
      return "native";
    case SimdBuildMode::kScalar:
      return "scalar";
  }

  return "unknown";
}

SimdBackend configuredSimdBackend() { return backend::selectedBackend(); }

const char* configuredSimdBackendName() { return backend::backendName(configuredSimdBackend()); }

}  // namespace tiny_skia::wide
