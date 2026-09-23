#include "donner/svg/renderer/geode/tests/GeodeTestContexts.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::geode {

namespace {

/// The running test's full name, or a note that no test is running.
std::string CurrentTestName() {
  const testing::TestInfo* test = testing::UnitTest::GetInstance()->current_test_info();
  if (test == nullptr) {
    return "code outside a test";
  }
  return std::string(test->test_suite_name()) + "." + test->name();
}

}  // namespace

std::unique_ptr<GeodeDevice> CreateTransitionalAdapterContext(std::string_view reason,
                                                              gpu::TextureFormat textureFormat) {
  // Only a request for another backend makes this worth saying. A malformed request is reported
  // by the selections that read it; this one names its backend instead.
  if (const gpu::Result<GpuBackendKind> processDefault = ProcessDefaultGpuBackendKind();
      !processDefault.hasError() && processDefault.result() != GpuBackendKind::TransitionalWgpu) {
    std::cerr << "[Geode] " << CurrentTestName()
              << " runs on the transitional wgpu adapter, not the process default "
              << processDefault.result() << ": " << reason << "\n";
  }
  GpuRootSelection selection;
  selection.label = "GeodeTransitionalAdapterTest";
  selection.backend = GpuBackendKind::TransitionalWgpu;
  std::shared_ptr<GeodeGpuRoot> root = SelectGpuRoot(selection);
  if (root == nullptr) {
    return nullptr;
  }
  return GeodeDevice::CreateOverSelectedRoot(std::move(root), textureFormat);
}

gpu::Result<std::vector<uint8_t>> ReadTexturePixels(gpu::Device& device,
                                                    const gpu::Texture& texture,
                                                    gpu::Extent2d extent) {
  constexpr uint32_t kBytesPerTexel = 4;
  const uint32_t rowBytes = extent.width * kBytesPerTexel;
  const uint32_t paddedRowBytes = AlignReadbackBytesPerRow(rowBytes);
  const uint64_t bufferBytes = static_cast<uint64_t>(paddedRowBytes) * extent.height;

  gpu::Result<gpu::Buffer> buffer = device.createBuffer(gpu::BufferDescriptor{
      "ReadTexturePixels", bufferBytes, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead});
  if (buffer.hasError()) {
    return std::move(buffer).error();
  }
  gpu::Result<std::unique_ptr<gpu::CommandEncoder>> encoder = device.createCommandEncoder();
  if (encoder.hasError()) {
    return std::move(encoder).error();
  }
  const gpu::TexelCopyBufferLayout layout{0, paddedRowBytes, extent.height};
  if (gpu::Status copied = encoder.result()->copyTextureToBuffer(gpu::TexelCopyTextureInfo{texture},
                                                                 buffer.result(), layout, extent);
      copied.hasError()) {
    return std::move(copied).error();
  }
  gpu::Result<gpu::CommandBuffer> commands = encoder.result()->finish();
  if (commands.hasError()) {
    return std::move(commands).error();
  }
  if (gpu::Result<uint64_t> submitted = device.submit(std::move(commands).result());
      submitted.hasError()) {
    return std::move(submitted).error();
  }

  gpu::Result<gpu::BufferMapping> mapping =
      device.mapBufferAsync(buffer.result(), gpu::MapMode::Read, 0, bufferBytes);
  if (mapping.hasError()) {
    return std::move(mapping).error();
  }
  gpu::Result<gpu::MapWaitReport> waited = device.waitForMapping(
      mapping.result(), gpu::MapWaitParams{.sliceSeconds = 0.005, .timeoutSeconds = 30.0}, {});
  if (waited.hasError()) {
    return std::move(waited).error();
  }
  if (waited.result().outcome != gpu::MapWaitOutcome::Ready) {
    std::ostringstream message;
    message << "ReadTexturePixels: the readback mapping ended " << waited.result().outcome;
    return gpu::GpuError{gpu::GpuErrorType::InvalidState, message.str()};
  }
  gpu::Result<std::span<const uint8_t>> bytes = device.mappedBytes(mapping.result());
  if (bytes.hasError()) {
    return std::move(bytes).error();
  }

  std::vector<uint8_t> pixels(static_cast<size_t>(rowBytes) * extent.height);
  for (uint32_t y = 0; y < extent.height; ++y) {
    const std::span<const uint8_t> row =
        bytes.result().subspan(static_cast<size_t>(y) * paddedRowBytes, rowBytes);
    std::copy(row.begin(), row.end(), pixels.begin() + static_cast<ptrdiff_t>(y) * rowBytes);
  }
  (void)device.unmapBuffer(std::move(mapping).result());
  return pixels;
}

}  // namespace donner::geode
