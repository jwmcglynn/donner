#include "donner/svg/renderer/geode/GeoEncoder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "donner/base/FillRule.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"

namespace donner::geode {

namespace {

constexpr uint32_t kSize = 64;
constexpr wgpu::TextureFormat kFormat = wgpu::TextureFormat::RGBA8Unorm;
constexpr uint32_t kBytesPerRow = 256;  // Padded from kSize*4 = 256.

/// Test fixture: creates a fresh device + render target + readback buffer per
/// test, and provides a helper to extract the rendered pixels.
class GeoEncoderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    device_ = GeodeDevice::CreateHeadless();
    ASSERT_NE(device_, nullptr);

    pipeline_ = std::make_unique<GeodePipeline>(device_->device(), kFormat);

    wgpu::TextureDescriptor td = {};
    td.label = "TestTarget";
    td.size = {kSize, kSize, 1};
    td.format = kFormat;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.dimension = wgpu::TextureDimension::e2D;
    target_ = device_->device().CreateTexture(&td);
    ASSERT_TRUE(static_cast<bool>(target_));

    wgpu::BufferDescriptor bd = {};
    bd.label = "TestReadback";
    bd.size = kBytesPerRow * kSize;
    bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    readback_ = device_->device().CreateBuffer(&bd);
    ASSERT_TRUE(static_cast<bool>(readback_));
  }

  /// Read back the rendered texture into a flat RGBA byte array (row-major,
  /// no padding — `kSize * kSize * 4` bytes).
  std::vector<uint8_t> readback() {
    // Copy texture → readback buffer.
    wgpu::CommandEncoder enc = device_->device().CreateCommandEncoder();

    wgpu::TexelCopyTextureInfo src = {};
    src.texture = target_;
    src.mipLevel = 0;
    src.origin = {0, 0, 0};

    wgpu::TexelCopyBufferInfo dst = {};
    dst.buffer = readback_;
    dst.layout.bytesPerRow = kBytesPerRow;
    dst.layout.rowsPerImage = kSize;

    wgpu::Extent3D copySize = {kSize, kSize, 1};
    enc.CopyTextureToBuffer(&src, &dst, &copySize);

    wgpu::CommandBuffer cmd = enc.Finish();
    device_->queue().Submit(1, &cmd);

    // Map readback buffer.
    bool mapDone = false;
    readback_.MapAsync(
        wgpu::MapMode::Read, 0, kBytesPerRow * kSize, wgpu::CallbackMode::AllowSpontaneous,
        [&mapDone](wgpu::MapAsyncStatus status, wgpu::StringView /*msg*/) {
          EXPECT_EQ(status, wgpu::MapAsyncStatus::Success);
          mapDone = true;
        });
    while (!mapDone) {
      device_->device().Tick();
    }

    const uint8_t* mapped = static_cast<const uint8_t*>(readback_.GetConstMappedRange());

    // Strip the row padding (256 bytes per row → 256 bytes per row, but the
    // visible part is kSize * 4 = 256 bytes for kSize=64, so no stripping
    // needed for our test size). Be defensive in case kSize ever changes.
    std::vector<uint8_t> pixels(kSize * kSize * 4);
    for (uint32_t y = 0; y < kSize; ++y) {
      std::copy_n(mapped + y * kBytesPerRow, kSize * 4, pixels.data() + y * kSize * 4);
    }
    readback_.Unmap();
    return pixels;
  }

  /// Get the RGBA value at pixel (x, y).
  static std::array<uint8_t, 4> pixelAt(const std::vector<uint8_t>& pixels, uint32_t x,
                                        uint32_t y) {
    const size_t off = (y * kSize + x) * 4;
    return {pixels[off], pixels[off + 1], pixels[off + 2], pixels[off + 3]};
  }

  std::unique_ptr<GeodeDevice> device_;
  std::unique_ptr<GeodePipeline> pipeline_;
  wgpu::Texture target_;
  wgpu::Buffer readback_;
};

// ----------------------------------------------------------------------------

/// Just clear and read back — simplest end-to-end test.
TEST_F(GeoEncoderTest, ClearOnly) {
  GeoEncoder encoder(*device_, *pipeline_, target_);
  encoder.clear(css::RGBA(0, 128, 255, 255));  // Half-saturated blue.
  encoder.finish();

  auto pixels = readback();
  auto pixel = pixelAt(pixels, 32, 32);
  EXPECT_EQ(pixel[0], 0u);
  EXPECT_EQ(pixel[1], 128u);
  EXPECT_EQ(pixel[2], 255u);
  EXPECT_EQ(pixel[3], 255u);
}

/// Fill an axis-aligned rectangle and verify a center pixel is the fill color.
TEST_F(GeoEncoderTest, FillRect) {
  Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();

  GeoEncoder encoder(*device_, *pipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));  // Black.
  encoder.fillPath(path, css::RGBA(255, 0, 0, 255), FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Center should be red.
  auto center = pixelAt(pixels, 32, 32);
  EXPECT_EQ(center[0], 255u) << "Center R";
  EXPECT_EQ(center[1], 0u) << "Center G";
  EXPECT_EQ(center[2], 0u) << "Center B";
  EXPECT_EQ(center[3], 255u) << "Center A";

  // Top-left corner should be black (outside the rect).
  auto corner = pixelAt(pixels, 4, 4);
  EXPECT_EQ(corner[0], 0u) << "Corner R";
  EXPECT_EQ(corner[1], 0u) << "Corner G";
  EXPECT_EQ(corner[2], 0u) << "Corner B";
  EXPECT_EQ(corner[3], 255u) << "Corner A";
}

/// Fill a triangle. Verify center inside, far corners outside.
TEST_F(GeoEncoderTest, FillTriangle) {
  Path path = PathBuilder()
                  .moveTo(Vector2d(32, 8))
                  .lineTo(Vector2d(56, 56))
                  .lineTo(Vector2d(8, 56))
                  .closePath()
                  .build();

  GeoEncoder encoder(*device_, *pipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPath(path, css::RGBA(0, 255, 0, 255), FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Center of triangle (32, 40) should be green.
  auto inside = pixelAt(pixels, 32, 40);
  EXPECT_EQ(inside[1], 255u) << "Triangle center G";

  // Top-left and top-right corners are outside the triangle.
  auto topLeft = pixelAt(pixels, 4, 4);
  EXPECT_EQ(topLeft[1], 0u) << "Top-left G";
  auto topRight = pixelAt(pixels, 60, 4);
  EXPECT_EQ(topRight[1], 0u) << "Top-right G";
}

/// Fill a circle. Verify center inside, far corners outside.
TEST_F(GeoEncoderTest, FillCircle) {
  Path path = PathBuilder().addCircle(Vector2d(32, 32), 20).build();

  GeoEncoder encoder(*device_, *pipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPath(path, css::RGBA(0, 0, 255, 255), FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Center should be blue.
  auto center = pixelAt(pixels, 32, 32);
  EXPECT_EQ(center[2], 255u) << "Center B";

  // Far corner should be black (outside the circle).
  auto corner = pixelAt(pixels, 2, 2);
  EXPECT_EQ(corner[2], 0u) << "Corner B";
}

}  // namespace

}  // namespace donner::geode
