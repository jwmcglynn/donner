#include "donner/svg/renderer/geode/GeodeShaders.h"

#include <gtest/gtest.h>

#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::geode {

/// Smoke test: the Slug fill shader compiles without errors.
/// If the WGSL has a syntax error or undefined symbol, shader module creation
/// fails and the test fails. The family creators go through the donner::gpu
/// runtime (design 0053 packet 8), so compilation runs on the wgpu adapter.
TEST(GeodeShaders, SlugFillCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createSlugFillShader(geodeDevice->adapterDevice());
  ASSERT_FALSE(module.hasError()) << "Slug fill shader failed to compile: " << module.error();

  // Note: Dawn's shader compilation is asynchronous in principle but for
  // WGSL it's typically synchronous. If this test passes the WGSL parsed
  // and type-checked successfully - errors would have fired the device's
  // uncaptured error callback before we get here.
}

/// Smoke test for the analytic gradient shader used on every adapter.
TEST(GeodeShaders, SlugGradientCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createSlugGradientShader(geodeDevice->adapterDevice());
  ASSERT_FALSE(module.hasError()) << "Slug gradient shader failed to compile: " << module.error();
}

/// Smoke test for the Phase 3b path-clip mask shader.
TEST(GeodeShaders, SlugMaskCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createSlugMaskShader(geodeDevice->adapterDevice());
  ASSERT_FALSE(module.hasError()) << "Slug mask shader failed to compile: " << module.error();
}

/// Smoke test for the image-blit shader shared by drawImage and the pattern path.
TEST(GeodeShaders, ImageBlitCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createImageBlitShader(geodeDevice->adapterDevice());
  ASSERT_FALSE(module.hasError()) << "Image blit shader failed to compile: " << module.error();
}

TEST(GeodeShaders, FilterDropShadowCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  wgpu::ShaderModule module = createFilterDropShadowShader(geodeDevice->device());
  ASSERT_TRUE(static_cast<bool>(module)) << "feDropShadow compose shader failed to compile";
}

TEST(GeodeShaders, FilterImageCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  wgpu::ShaderModule module = createFilterImageShader(geodeDevice->device());
  ASSERT_TRUE(static_cast<bool>(module)) << "feImage shader failed to compile";
}

TEST(GeodeShaders, FilterTileCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  wgpu::ShaderModule module = createFilterTileShader(geodeDevice->device());
  ASSERT_TRUE(static_cast<bool>(module)) << "feTile shader failed to compile";
}

}  // namespace donner::geode
