#include "donner/svg/renderer/geode/GeodeShaders.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "donner/gpu/tests/SlugFillSlice.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeShaderSelection.h"

namespace donner::geode {
namespace {
gpu::Result<std::vector<uint8_t>> ReadSlugBuffer(gpu::Device& device, const gpu::Buffer& buffer) {
  auto mapping = device.mapBufferAsync(buffer, gpu::MapMode::Read, 0, 2048);
  if (mapping.hasError()) {
    return mapping.error();
  }
  const auto waited = device.waitForMapping(mapping.result(), {0.01, 5.0}, {});
  if (waited.hasError()) {
    return waited.error();
  }
  if (waited.result().outcome != gpu::MapWaitOutcome::Ready) {
    return gpu::GpuError{gpu::GpuErrorType::InvalidState, "Slug readback did not complete"};
  }
  const auto bytes = device.mappedBytes(mapping.result());
  if (bytes.hasError()) {
    return bytes.error();
  }
  std::vector<uint8_t> result(bytes.result().begin(), bytes.result().end());
  const auto unmapped = device.unmapBuffer(std::move(mapping).result());
  if (unmapped.hasError()) {
    return unmapped.error();
  }
  return result;
}
}  // namespace
TEST(GeodeShaders, SlugFillReferenceEvenOdd) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);
  gpu::Device& runtime = device->runtimeDevice();
  gpu::tests::CheckSlugFill(
      runtime, SelectShaderProjection(runtime, DONNER_GEODE_SHADER_ARTIFACTS(SlugFill)),
      [&](const gpu::Buffer& b) { return ReadSlugBuffer(runtime, b); },
      gpu::tests::slug_fill_slice::Case::EvenOdd);
}
TEST(GeodeShaders, SlugFillReferenceLinearGradient) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);
  gpu::Device& runtime = device->runtimeDevice();
  gpu::tests::CheckSlugFill(
      runtime, SelectShaderProjection(runtime, DONNER_GEODE_SHADER_ARTIFACTS(SlugFill)),
      [&](const gpu::Buffer& b) { return ReadSlugBuffer(runtime, b); },
      gpu::tests::slug_fill_slice::Case::LinearGradient);
}

/// Smoke test: the native backend creates the Slug fill module from its frozen projection.
TEST(GeodeShaders, SlugFillCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createSlugFillShader(geodeDevice->runtimeDevice());
  ASSERT_FALSE(module.hasError()) << "Slug fill shader failed to compile: " << module.error();
}

/// Smoke test for the native analytic gradient shader.
TEST(GeodeShaders, SlugGradientCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createSlugGradientShader(geodeDevice->runtimeDevice());
  ASSERT_FALSE(module.hasError()) << "Slug gradient shader failed to compile: " << module.error();
}

/// Smoke test for the path-clip mask shader.
TEST(GeodeShaders, SlugMaskCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createSlugMaskShader(geodeDevice->runtimeDevice());
  ASSERT_FALSE(module.hasError()) << "Slug mask shader failed to compile: " << module.error();
}

/// Smoke test for the image-blit shader shared by drawImage and the pattern path.
TEST(GeodeShaders, ImageBlitCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createImageBlitShader(geodeDevice->runtimeDevice());
  ASSERT_FALSE(module.hasError()) << "Image blit shader failed to compile: " << module.error();
}

}  // namespace donner::geode
