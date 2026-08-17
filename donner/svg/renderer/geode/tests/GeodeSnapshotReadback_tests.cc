/// @file
/// Byte-parity tests for GPU-side snapshot unpremultiplication.
///
/// The GPU readback path (compute unpremultiply into a straight-alpha staging
/// texture, then a texture-to-buffer copy) must produce byte-identical output
/// to the CPU copy path. Each path is forced by controlling texture usage
/// flags: the GPU path requires TextureBinding, the CPU path requires CopySrc.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"  // IWYU pragma: keep - provides wgpuLabel

namespace donner::svg {

using ::donner::geode::wgpuLabel;

namespace {

constexpr uint32_t kWidth = 8;
constexpr uint32_t kHeight = 4;

/// Premultiplied RGBA test pixels covering every CPU unpremultiply branch:
/// a == 0 with nonzero RGB, a == 255, odd and even partial alpha with
/// nontrivial round-half-up behavior.
const std::array<uint8_t, kWidth * kHeight * 4>& premultipliedTestPixels() {
  static const std::array<uint8_t, kWidth * kHeight * 4> pixels = [] {
    std::array<uint8_t, kWidth * kHeight * 4> p = {};
    auto set = [&p](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
      const size_t off = (static_cast<size_t>(y) * kWidth + x) * 4u;
      p[off + 0] = r;
      p[off + 1] = g;
      p[off + 2] = b;
      p[off + 3] = a;
    };
    // Row 0: fully transparent with nonzero premultiplied RGB (must zero out).
    set(0, 0, 255, 128, 64, 0);
    set(1, 0, 17, 9, 3, 0);
    // Row 1: fully opaque (straight copy).
    set(1, 1, 200, 100, 50, 255);
    set(2, 1, 3, 240, 255, 255);
    // Row 2: odd partial alpha, round-half-up cases.
    set(2, 2, 2, 1, 1, 3);
    set(3, 2, 200, 128, 64, 128);
    set(4, 2, 7, 6, 5, 7);
    // Row 3: even partial alpha and a == 1 edge case.
    set(4, 3, 77, 33, 11, 64);
    set(5, 3, 255, 255, 255, 1);
    return p;
  }();
  return pixels;
}

/// Create a texture with the given usage flags and upload the test pixels.
wgpu::Texture createTestTexture(const wgpu::Device& device, wgpu::TextureUsage usage) {
  wgpu::TextureDescriptor desc = {};
  desc.label = wgpuLabel("SnapshotReadbackParity");
  desc.size = {kWidth, kHeight, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = usage | wgpu::TextureUsage::CopyDst;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  desc.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture texture = device.createTexture(desc);

  wgpu::TexelCopyTextureInfo destination = {};
  destination.texture = texture;
  wgpu::TexelCopyBufferLayout dataLayout = {};
  dataLayout.bytesPerRow = kWidth * 4u;
  dataLayout.rowsPerImage = kHeight;
  wgpu::Extent3D writeSize = {kWidth, kHeight, 1};
  device.getQueue().writeTexture(destination, premultipliedTestPixels().data(),
                                 premultipliedTestPixels().size(), dataLayout, writeSize);
  return texture;
}

class GeodeSnapshotReadbackTest : public ::testing::Test {
protected:
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }();
    return device;
  }
};

/// The GPU unpremultiply path (TextureBinding, no CopySrc) and the CPU copy
/// path (CopySrc, no TextureBinding) must produce byte-identical straight-alpha
/// output for the same premultiplied input. The usage flags force each path:
/// the GPU path cannot run without TextureBinding, and the CPU copy cannot run
/// without CopySrc, so a mismatch between the two bitmaps cannot be hidden by
/// silent path fallback.
TEST_F(GeodeSnapshotReadbackTest, GpuAndCpuPathsAreByteIdentical) {
  auto device = sharedDevice();
  ASSERT_NE(device, nullptr);
  ASSERT_TRUE(device->snapshotReadbackPipeline().valid());

  wgpu::Texture gpuTex = createTestTexture(device->device(), wgpu::TextureUsage::TextureBinding);
  wgpu::Texture cpuTex = createTestTexture(device->device(), wgpu::TextureUsage::CopySrc);

  const Vector2i dimensions(static_cast<int>(kWidth), static_cast<int>(kHeight));
  RendererGeodeTextureSnapshot gpuSnapshot(device, std::move(gpuTex), dimensions,
                                           wgpu::TextureFormat::RGBA8Unorm);
  RendererGeodeTextureSnapshot cpuSnapshot(device, std::move(cpuTex), dimensions,
                                           wgpu::TextureFormat::RGBA8Unorm);

  RendererBitmap gpuBitmap = gpuSnapshot.takeSnapshot();
  ASSERT_FALSE(gpuBitmap.empty());
  RendererBitmap cpuBitmap = cpuSnapshot.takeSnapshot();
  ASSERT_FALSE(cpuBitmap.empty());

  EXPECT_EQ(gpuBitmap.dimensions, dimensions);
  EXPECT_EQ(gpuBitmap.alphaType, AlphaType::Unpremultiplied);
  EXPECT_EQ(gpuBitmap.pixels, cpuBitmap.pixels)
      << "GPU unpremultiply output differs from the CPU reference path";
}

}  // namespace
}  // namespace donner::svg
