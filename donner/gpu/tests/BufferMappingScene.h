#pragma once
/// @file
/// The host-mapping contract a native backend must satisfy, written once and run by both the
/// Metal and the Vulkan execution suites so the two cannot drift apart.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {

/// Texels per side of the mapped scene.
inline constexpr uint32_t kMappingSceneExtent = 2;
/// Row stride of the mapped scene; texture-to-buffer copies require 256-byte alignment.
inline constexpr uint32_t kMappingSceneBytesPerRow = 256;
/// Total bytes of the mapped scene.
inline constexpr uint64_t kMappingSceneByteSize =
    uint64_t{kMappingSceneBytesPerRow} * kMappingSceneExtent;

/// Texel at (\p x, \p y) of the scene: four values that differ in every channel, so a row-stride
/// or channel-order mistake cannot read as a pass.
/// @param x Column. @param y Row.
inline std::array<uint8_t, 4> MappingSceneTexel(uint32_t x, uint32_t y) {
  const uint8_t base = static_cast<uint8_t>(0x10 + (y * kMappingSceneExtent + x) * 0x20);
  return {base, static_cast<uint8_t>(base + 1), static_cast<uint8_t>(base + 2), 0xFF};
}

/// The scene's texels laid out at \ref kMappingSceneBytesPerRow, ready for `writeTexture`.
inline std::vector<uint8_t> MappingSceneUpload() {
  std::vector<uint8_t> upload(static_cast<size_t>(kMappingSceneByteSize), 0);
  for (uint32_t y = 0; y < kMappingSceneExtent; ++y) {
    for (uint32_t x = 0; x < kMappingSceneExtent; ++x) {
      const std::array<uint8_t, 4> texel = MappingSceneTexel(x, y);
      std::copy(texel.begin(), texel.end(),
                upload.begin() + static_cast<ptrdiff_t>(y * kMappingSceneBytesPerRow + x * 4u));
    }
  }
  return upload;
}

/// A readback buffer filled by a submitted texture-to-buffer copy, plus that submission's serial.
struct MappingScene {
  Texture texture;      //!< Source texture holding \ref MappingSceneUpload.
  Buffer readback;      //!< Map-readable destination of the copy.
  uint64_t serial = 0;  //!< Serial of the submission that fills \ref readback.
};

/// Creates a map-readable buffer of \p byteSize on \p device.
/// @param device Device to create on. @param byteSize Size in bytes.
inline Buffer MakeReadbackBuffer(Device& device, uint64_t byteSize = kMappingSceneByteSize) {
  return GetResultOrFail(device.createBuffer(
      BufferDescriptor{"mappingScene", byteSize, BufferUsage::CopyDst | BufferUsage::MapRead}));
}

/// Submits a texture-to-buffer copy of the scene and returns its resources without waiting.
///
/// @param device Device to build the scene on.
/// @param scene Filled in with the scene's resources and submission serial.
inline void BuildMappingScene(Device& device, MappingScene& scene) {
  const Extent2d extent{kMappingSceneExtent, kMappingSceneExtent};
  const TexelCopyBufferLayout layout{0, kMappingSceneBytesPerRow, kMappingSceneExtent};

  scene.texture = GetResultOrFail(
      device.createTexture(TextureDescriptor{"mappingScene", extent, TextureFormat::RGBA8Unorm,
                                             TextureUsage::CopySrc | TextureUsage::CopyDst}));
  scene.readback = MakeReadbackBuffer(device);
  ASSERT_THAT(device.writeTexture(scene.texture, MappingSceneUpload(), layout, extent), IsOk());

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device.createCommandEncoder());
  ASSERT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{scene.texture}, scene.readback,
                                           layout, extent),
              IsOk());
  scene.serial = GetResultOrFail(device.submit(GetResultOrFail(encoder->finish())));
}

/// Asserts \p bytes holds the scene's texels. @param bytes Mapped range. @param label Context.
inline void ExpectSceneTexels(std::span<const uint8_t> bytes, std::string_view label) {
  ASSERT_GE(bytes.size(), kMappingSceneByteSize) << label;
  for (uint32_t y = 0; y < kMappingSceneExtent; ++y) {
    for (uint32_t x = 0; x < kMappingSceneExtent; ++x) {
      const size_t offset = y * kMappingSceneBytesPerRow + x * 4u;
      const std::array<uint8_t, 4> actual = {bytes[offset], bytes[offset + 1], bytes[offset + 2],
                                             bytes[offset + 3]};
      EXPECT_THAT(actual, testing::ElementsAreArray(MappingSceneTexel(x, y)))
          << label << " texel (" << x << ", " << y << ")";
    }
  }
}

/// A wait long enough for a copy of this size on any supported device.
inline MapWaitParams SceneWaitParams() {
  return MapWaitParams{0.005, 30.0};
}

/// The central contract: a mapping taken right after submitting the copy waits for that
/// submission by itself and then reads the bytes the copy produced.
///
/// Nothing waits for the serial first, so a backend that ignored the submission and reported the
/// mapping ready immediately would hand back an unwritten buffer and fail here.
///
/// @param device Device under test.
inline void ExpectMappingWaitsForItsSubmission(Device& device) {
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(BuildMappingScene(device, scene));

  BufferMapping mapping = GetResultOrFail(
      device.mapBufferAsync(scene.readback, MapMode::Read, 0, kMappingSceneByteSize));
  ASSERT_EQ(GetResultOrFail(device.waitForMapping(mapping, SceneWaitParams(), {})),
            MapWaitOutcome::Ready);

  ASSERT_NO_FATAL_FAILURE(
      ExpectSceneTexels(GetResultOrFail(device.mappedBytes(mapping)), "mapped copy"));
  EXPECT_THAT(device.unmapBuffer(std::move(mapping)), IsOk());
}

/// A mapped subrange names its own bytes, not the start of the buffer.
/// @param device Device under test.
inline void ExpectSubrangeMappingReadsItsOwnBytes(Device& device) {
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(BuildMappingScene(device, scene));

  // The second row of the copy, which holds the texels of scene row 1.
  BufferMapping mapping = GetResultOrFail(device.mapBufferAsync(
      scene.readback, MapMode::Read, kMappingSceneBytesPerRow, kMappingSceneBytesPerRow));
  ASSERT_EQ(GetResultOrFail(device.waitForMapping(mapping, SceneWaitParams(), {})),
            MapWaitOutcome::Ready);

  const std::span<const uint8_t> bytes = GetResultOrFail(device.mappedBytes(mapping));
  ASSERT_EQ(bytes.size(), kMappingSceneBytesPerRow);
  const std::array<uint8_t, 4> first = {bytes[0], bytes[1], bytes[2], bytes[3]};
  EXPECT_THAT(first, testing::ElementsAreArray(MappingSceneTexel(0, 1)))
      << "A subrange mapping must start at the offset it named";
  EXPECT_THAT(device.unmapBuffer(std::move(mapping)), IsOk());
}

/// Reading before a wait has seen the mapping complete is refused.
/// @param device Device under test.
inline void ExpectReadBeforeCompletionIsRefused(Device& device) {
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(BuildMappingScene(device, scene));

  BufferMapping mapping = GetResultOrFail(
      device.mapBufferAsync(scene.readback, MapMode::Read, 0, kMappingSceneByteSize));

  EXPECT_THAT(device.mappedBytes(mapping), IsGpuError(GpuErrorType::InvalidState))
      << "A mapping that has not been waited for must not hand back bytes";
  EXPECT_THAT(device.unmapBuffer(std::move(mapping)), IsOk());
}

/// One buffer carries one mapping at a time, and a released mapping frees it for the next.
/// @param device Device under test.
inline void ExpectOneMappingPerBuffer(Device& device) {
  const Buffer readback = MakeReadbackBuffer(device);

  BufferMapping mapping =
      GetResultOrFail(device.mapBufferAsync(readback, MapMode::Read, 0, kMappingSceneByteSize));
  EXPECT_THAT(device.mapBufferAsync(readback, MapMode::Read, 0, kMappingSceneByteSize),
              IsGpuError(GpuErrorType::InvalidState));

  ASSERT_THAT(device.unmapBuffer(std::move(mapping)), IsOk());
  BufferMapping second =
      GetResultOrFail(device.mapBufferAsync(readback, MapMode::Read, 0, kMappingSceneByteSize));
  EXPECT_THAT(device.unmapBuffer(std::move(second)), IsOk());
}

/// Releasing a mapping ends access through it, and releasing it again is reported.
/// @param device Device under test.
inline void ExpectUnmapEndsAccess(Device& device) {
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(BuildMappingScene(device, scene));

  BufferMapping mapping = GetResultOrFail(
      device.mapBufferAsync(scene.readback, MapMode::Read, 0, kMappingSceneByteSize));
  ASSERT_EQ(GetResultOrFail(device.waitForMapping(mapping, SceneWaitParams(), {})),
            MapWaitOutcome::Ready);
  ASSERT_THAT(device.mappedBytes(mapping), HasResult());

  ASSERT_THAT(device.unmapBuffer(std::move(mapping)), IsOk());

  EXPECT_THAT(device.mappedBytes(mapping), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device.unmapBuffer(std::move(mapping)), IsGpuError(GpuErrorType::InvalidHandle))
      << "Releasing an already released mapping must be reported, not repeated";
}

/// Destroying the buffer invalidates an open mapping instead of keeping the allocation alive.
/// @param device Device under test.
inline void ExpectDestroyedBufferInvalidatesMapping(Device& device) {
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(BuildMappingScene(device, scene));

  BufferMapping mapping = GetResultOrFail(
      device.mapBufferAsync(scene.readback, MapMode::Read, 0, kMappingSceneByteSize));
  ASSERT_EQ(GetResultOrFail(device.waitForMapping(mapping, SceneWaitParams(), {})),
            MapWaitOutcome::Ready);

  ASSERT_THAT(device.destroyBuffer(std::move(scene.readback)), IsOk());

  EXPECT_THAT(device.mappedBytes(mapping), IsGpuError(GpuErrorType::InvalidHandle))
      << "A mapping must not read a buffer that has been destroyed";
  EXPECT_THAT(device.waitForMapping(mapping, SceneWaitParams(), {}),
              IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device.unmapBuffer(std::move(mapping)), IsOk());
}

/// A range past the end of the buffer is refused before anything is mapped.
/// @param device Device under test.
inline void ExpectRangePastTheEndIsRefused(Device& device) {
  const Buffer readback = MakeReadbackBuffer(device);

  EXPECT_THAT(device.mapBufferAsync(readback, MapMode::Read, 4, kMappingSceneByteSize),
              IsGpuError(GpuErrorType::OutOfBounds));
  EXPECT_THAT(device.mapBufferAsync(readback, MapMode::Read, 0, 0),
              IsGpuError(GpuErrorType::InvalidDescriptor));
}

/// A buffer without the MapRead usage cannot be mapped.
/// @param device Device under test.
inline void ExpectMapReadUsageIsRequired(Device& device) {
  const Buffer plain = GetResultOrFail(device.createBuffer(BufferDescriptor{
      "plain", kMappingSceneByteSize, BufferUsage::CopyDst | BufferUsage::Vertex}));

  EXPECT_THAT(device.mapBufferAsync(plain, MapMode::Read, 0, kMappingSceneByteSize),
              IsGpuError(GpuErrorType::UsageMismatch));
}

}  // namespace donner::gpu::tests
