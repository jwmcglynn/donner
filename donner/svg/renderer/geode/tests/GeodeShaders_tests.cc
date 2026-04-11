#include "donner/svg/renderer/geode/GeodeShaders.h"

#include <gtest/gtest.h>

#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::geode {

/// Smoke test: the Slug fill shader compiles without errors.
/// If the WGSL has a syntax error or undefined symbol, shader module creation
/// fails and the test fails.
TEST(GeodeShaders, SlugFillCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  wgpu::ShaderModule module = createSlugFillShader(geodeDevice->device());
  ASSERT_TRUE(static_cast<bool>(module)) << "Slug fill shader failed to compile";

  // Note: Dawn's shader compilation is asynchronous in principle but for
  // WGSL it's typically synchronous. If this test passes the WGSL parsed
  // and type-checked successfully — errors would have fired the device's
  // uncaptured error callback before we get here.
}

/// Smoke test for the Phase 3b path-clip mask shader.
TEST(GeodeShaders, SlugMaskCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  wgpu::ShaderModule module = createSlugMaskShader(geodeDevice->device());
  ASSERT_TRUE(static_cast<bool>(module)) << "Slug mask shader failed to compile";
}

}  // namespace donner::geode
