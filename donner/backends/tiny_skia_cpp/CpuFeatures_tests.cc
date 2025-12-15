/// @file
/// Unit tests for runtime CPU feature detection.

#include "donner/backends/tiny_skia_cpp/CpuFeatures.h"

#include "gtest/gtest.h"

namespace donner::backends::tiny_skia_cpp {

TEST(CpuFeaturesTest, DetectsArchitectureCapabilities) {
  const CpuFeatures& features = GetCpuFeatures();

#if defined(__aarch64__) || defined(__ARM_NEON)
  EXPECT_TRUE(features.hasNeon);
#else
  EXPECT_FALSE(features.hasNeon);
#endif

#if defined(__SSE2__)
  EXPECT_TRUE(features.hasSse2);
#else
  EXPECT_FALSE(features.hasSse2);
#endif

#if defined(__AVX2__)
  EXPECT_TRUE(features.hasAvx2);
#endif

#if defined(__AVX2__)
  EXPECT_TRUE(features.hasSse2);
#endif
}

}  // namespace donner::backends::tiny_skia_cpp
