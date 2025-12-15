/// @file
/// Runtime CPU feature detection helpers for tiny_skia_cpp SIMD dispatch.

#include "donner/backends/tiny_skia_cpp/CpuFeatures.h"

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace donner::backends::tiny_skia_cpp {

namespace {

CpuFeatures DetectCpuFeatures() {
  CpuFeatures features{};

#if defined(__x86_64__) || defined(__i386__)
  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;

  if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0) {
    features.hasSse2 = (edx & bit_SSE2) != 0;

    const bool osxsave = (ecx & bit_OSXSAVE) != 0;
    const bool avx = (ecx & bit_AVX) != 0;
    if (osxsave && avx) {
#if defined(__XSAVE__)
      const std::uint64_t xcr0 = _xgetbv(0);
      const bool ymmStateEnabled = (xcr0 & 0x6) == 0x6;
      if (ymmStateEnabled && __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) != 0) {
        features.hasAvx2 = (ebx & bit_AVX2) != 0;
      }
#endif
    }
  }
#elif defined(__aarch64__) || defined(__ARM_NEON)
  features.hasNeon = true;
#endif

  return features;
}

}  // namespace

const CpuFeatures& GetCpuFeatures() {
  static const CpuFeatures kFeatures = DetectCpuFeatures();
  return kFeatures;
}

}  // namespace donner::backends::tiny_skia_cpp
