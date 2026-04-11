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
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/resources/ImageResource.h"

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
    gradientPipeline_ =
        std::make_unique<GeodeGradientPipeline>(device_->device(), kFormat);
    imagePipeline_ = std::make_unique<GeodeImagePipeline>(device_->device(), kFormat);

    wgpu::TextureDescriptor td = {};
    td.label = "TestTarget";
    td.size = {kSize, kSize, 1};
    td.format = kFormat;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::TextureBinding;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.dimension = wgpu::TextureDimension::e2D;
    target_ = device_->device().CreateTexture(&td);
    ASSERT_TRUE(static_cast<bool>(target_));

    // 4× MSAA companion required by the GeoEncoder constructor. Pipelines
    // are created with `multisample.count = 4` so every render pass
    // attaches an MSAA color target that resolves into `target_`.
    wgpu::TextureDescriptor msaaDesc = {};
    msaaDesc.label = "TestTargetMSAA";
    msaaDesc.size = {kSize, kSize, 1};
    msaaDesc.format = kFormat;
    msaaDesc.usage = wgpu::TextureUsage::RenderAttachment;
    msaaDesc.mipLevelCount = 1;
    msaaDesc.sampleCount = 4;
    msaaDesc.dimension = wgpu::TextureDimension::e2D;
    msaaTarget_ = device_->device().CreateTexture(&msaaDesc);
    ASSERT_TRUE(static_cast<bool>(msaaTarget_));

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
  std::unique_ptr<GeodeGradientPipeline> gradientPipeline_;
  std::unique_ptr<GeodeImagePipeline> imagePipeline_;
  wgpu::Texture target_;
  wgpu::Texture msaaTarget_;
  wgpu::Buffer readback_;
};

// ----------------------------------------------------------------------------

/// Just clear and read back — simplest end-to-end test.
TEST_F(GeoEncoderTest, ClearOnly) {
  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
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

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
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

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
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

/// Fill a rectangle with a horizontal red→blue linear gradient in user space.
/// Left edge should be red, right edge should be blue, midpoint between.
/// Regression guard for the linear-gradient pipeline plumbing.
TEST_F(GeoEncoderTest, FillLinearGradientUserSpace) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {64, 64})).build();

  LinearGradientParams::Stop stops[2] = {};
  // Red at t=0.
  stops[0].offset = 0.0f;
  stops[0].rgba[0] = 1.0f;
  stops[0].rgba[1] = 0.0f;
  stops[0].rgba[2] = 0.0f;
  stops[0].rgba[3] = 1.0f;
  // Blue at t=1.
  stops[1].offset = 1.0f;
  stops[1].rgba[0] = 0.0f;
  stops[1].rgba[1] = 0.0f;
  stops[1].rgba[2] = 1.0f;
  stops[1].rgba[3] = 1.0f;

  LinearGradientParams params;
  params.startGrad = Vector2d(0.0, 0.0);
  params.endGrad = Vector2d(64.0, 0.0);
  params.gradientFromPath = Transform2d();  // Identity: path space == gradient space.
  params.spreadMode = 0;                    // pad
  params.stops = std::span<const LinearGradientParams::Stop>(stops, 2);

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPathLinearGradient(path, params, FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Left edge: almost all red.
  auto left = pixelAt(pixels, 1, 32);
  EXPECT_GT(left[0], 240u) << "Left edge R";
  EXPECT_LT(left[2], 16u) << "Left edge B";

  // Right edge: almost all blue.
  auto right = pixelAt(pixels, 62, 32);
  EXPECT_LT(right[0], 16u) << "Right edge R";
  EXPECT_GT(right[2], 240u) << "Right edge B";

  // Midpoint: ~50/50 red/blue.
  auto mid = pixelAt(pixels, 32, 32);
  EXPECT_NEAR(mid[0], 128u, 20u) << "Mid R";
  EXPECT_NEAR(mid[2], 128u, 20u) << "Mid B";
}

/// Gradient `spreadMode=repeat`: start=(16, 0) end=(32, 0) sampled across a
/// 64-wide rect. Outside [16, 32] the gradient should repeat, so pixel at
/// x=16 and x=48 should be (approximately) the same color.
TEST_F(GeoEncoderTest, FillLinearGradientRepeat) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {64, 64})).build();

  LinearGradientParams::Stop stops[2] = {};
  stops[0].offset = 0.0f;
  stops[0].rgba[0] = 1.0f;
  stops[0].rgba[3] = 1.0f;
  stops[1].offset = 1.0f;
  stops[1].rgba[2] = 1.0f;
  stops[1].rgba[3] = 1.0f;

  LinearGradientParams params;
  params.startGrad = Vector2d(16.0, 0.0);
  params.endGrad = Vector2d(32.0, 0.0);
  params.gradientFromPath = Transform2d();
  params.spreadMode = 2;  // repeat
  params.stops = std::span<const LinearGradientParams::Stop>(stops, 2);

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPathLinearGradient(path, params, FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // x=17 and x=33 should both sit at t ≈ (1/16) in their respective periods.
  auto a = pixelAt(pixels, 17, 32);
  auto b = pixelAt(pixels, 33, 32);
  EXPECT_NEAR(a[0], b[0], 8u) << "Repeat period mismatch R";
  EXPECT_NEAR(a[2], b[2], 8u) << "Repeat period mismatch B";
}

/// Concentric radial gradient (white center → black rim) over a 64x64 rect.
/// Center pixel must be (nearly) white, rim pixels must be (nearly) black.
TEST_F(GeoEncoderTest, FillRadialGradientConcentric) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {64, 64})).build();

  RadialGradientParams::Stop stops[2] = {};
  // White at t=0.
  stops[0].offset = 0.0f;
  stops[0].rgba[0] = 1.0f;
  stops[0].rgba[1] = 1.0f;
  stops[0].rgba[2] = 1.0f;
  stops[0].rgba[3] = 1.0f;
  // Black at t=1.
  stops[1].offset = 1.0f;
  stops[1].rgba[0] = 0.0f;
  stops[1].rgba[1] = 0.0f;
  stops[1].rgba[2] = 0.0f;
  stops[1].rgba[3] = 1.0f;

  RadialGradientParams params;
  params.center = Vector2d(32.0, 32.0);
  params.focalCenter = Vector2d(32.0, 32.0);
  params.radius = 32.0;
  params.focalRadius = 0.0;
  params.gradientFromPath = Transform2d();
  params.spreadMode = 0;  // pad
  params.stops = std::span<const RadialGradientParams::Stop>(stops, 2);

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPathRadialGradient(path, params, FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Center pixel (32, 32) sits exactly at the focal point → t == 0 → white.
  auto center = pixelAt(pixels, 32, 32);
  EXPECT_GT(center[0], 240u) << "Center R";
  EXPECT_GT(center[1], 240u) << "Center G";
  EXPECT_GT(center[2], 240u) << "Center B";

  // A point one pixel inside the rim (left edge) sits very close to t == 1
  // along the +x ray from the center → essentially black.
  auto leftRim = pixelAt(pixels, 1, 32);
  EXPECT_LT(leftRim[0], 32u) << "Left rim R";
  EXPECT_LT(leftRim[1], 32u) << "Left rim G";
  EXPECT_LT(leftRim[2], 32u) << "Left rim B";
}

/// Off-center focal point: brightest pixel should be at the focal point,
/// not at the geometric center of the outer circle.
TEST_F(GeoEncoderTest, FillRadialGradientFocal) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {64, 64})).build();

  RadialGradientParams::Stop stops[2] = {};
  stops[0].offset = 0.0f;
  stops[0].rgba[0] = 1.0f;
  stops[0].rgba[1] = 1.0f;
  stops[0].rgba[2] = 1.0f;
  stops[0].rgba[3] = 1.0f;
  stops[1].offset = 1.0f;
  stops[1].rgba[0] = 0.0f;
  stops[1].rgba[1] = 0.0f;
  stops[1].rgba[2] = 0.0f;
  stops[1].rgba[3] = 1.0f;

  RadialGradientParams params;
  params.center = Vector2d(32.0, 32.0);
  params.focalCenter = Vector2d(48.0, 32.0);  // Focal shifted toward +x.
  params.radius = 30.0;
  params.focalRadius = 0.0;
  params.gradientFromPath = Transform2d();
  params.spreadMode = 0;
  params.stops = std::span<const RadialGradientParams::Stop>(stops, 2);

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPathRadialGradient(path, params, FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // The pixel right at the focal point should be the lightest. The
  // geometric center of the outer circle should be noticeably darker.
  auto focal = pixelAt(pixels, 48, 32);
  auto geomCenter = pixelAt(pixels, 32, 32);
  EXPECT_GT(focal[0], geomCenter[0] + 32u) << "Focal lighter than center R";
  EXPECT_GT(focal[1], geomCenter[1] + 32u) << "Focal lighter than center G";
  EXPECT_GT(focal[2], geomCenter[2] + 32u) << "Focal lighter than center B";
}

/// Fill a circle. Verify center inside, far corners outside.
TEST_F(GeoEncoderTest, FillCircle) {
  Path path = PathBuilder().addCircle(Vector2d(32, 32), 20).build();

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
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

/// Build a 2x2 solid-magenta straight-alpha RGBA8 image resource.
svg::ImageResource makeMagentaImage2x2() {
  svg::ImageResource image;
  image.width = 2;
  image.height = 2;
  image.data = {
      255, 0, 255, 255, 255, 0, 255, 255,  //
      255, 0, 255, 255, 255, 0, 255, 255,
  };
  return image;
}

/// Draw a 2x2 magenta image stretched to fill a large destination rectangle.
/// Verify that the destination pixels are magenta and the outside is
/// untouched.
TEST_F(GeoEncoderTest, DrawImageFillsDestRect) {
  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));

  svg::ImageResource image = makeMagentaImage2x2();
  encoder.drawImage(image, Box2d({16.0, 16.0}, {48.0, 48.0}),
                    /*opacity=*/1.0, /*pixelated=*/true);
  encoder.finish();

  auto pixels = readback();
  auto center = pixelAt(pixels, 32, 32);
  EXPECT_EQ(center[0], 255u) << "Center R";
  EXPECT_EQ(center[1], 0u) << "Center G";
  EXPECT_EQ(center[2], 255u) << "Center B";
  EXPECT_EQ(center[3], 255u) << "Center A";

  // Outside the destRect is still the clear color (black).
  auto outside = pixelAt(pixels, 4, 4);
  EXPECT_EQ(outside[0], 0u) << "Outside R";
  EXPECT_EQ(outside[1], 0u) << "Outside G";
  EXPECT_EQ(outside[2], 0u) << "Outside B";
}

/// `opacity` should blend the image with whatever the pass already
/// contains. Start with a black background, draw a red image at 50%
/// opacity — the result should read back as ~(128, 0, 0, 255) over black
/// with premultiplied source-over.
TEST_F(GeoEncoderTest, DrawImageHonorsOpacity) {
  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));

  svg::ImageResource image;
  image.width = 1;
  image.height = 1;
  image.data = {255, 0, 0, 255};  // Red, full alpha.

  encoder.drawImage(image, Box2d({16.0, 16.0}, {48.0, 48.0}),
                    /*opacity=*/0.5, /*pixelated=*/false);
  encoder.finish();

  auto pixels = readback();
  auto center = pixelAt(pixels, 32, 32);
  // 0.5 alpha source over opaque black → R ≈ 128, A = 255.
  EXPECT_NEAR(center[0], 128u, 2u) << "Blended R";
  EXPECT_EQ(center[1], 0u) << "G";
  EXPECT_EQ(center[2], 0u) << "B";
  EXPECT_EQ(center[3], 255u) << "A";
}

/// Mixing a fillPath and a drawImage in the same pass must work — after
/// this test shipped, future refactors that forget to re-bind the Slug
/// fill pipeline between pipeline switches will regress here.
TEST_F(GeoEncoderTest, FillThenImageThenFill) {
  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));

  // 1) Fill a red rect covering the top half.
  Path topRect = PathBuilder().addRect(Box2d({0, 0}, {64, 32})).build();
  encoder.fillPath(topRect, css::RGBA(255, 0, 0, 255), FillRule::NonZero);

  // 2) Draw a magenta image in the center.
  svg::ImageResource image = makeMagentaImage2x2();
  encoder.drawImage(image, Box2d({24.0, 24.0}, {40.0, 40.0}),
                    /*opacity=*/1.0, /*pixelated=*/true);

  // 3) Fill a green rect covering the bottom strip (below the image).
  Path bottomRect = PathBuilder().addRect(Box2d({0, 48}, {64, 64})).build();
  encoder.fillPath(bottomRect, css::RGBA(0, 255, 0, 255), FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Top half: red.
  auto top = pixelAt(pixels, 8, 8);
  EXPECT_EQ(top[0], 255u) << "Top R";
  EXPECT_EQ(top[1], 0u) << "Top G";

  // Center (image): magenta.
  auto mid = pixelAt(pixels, 32, 32);
  EXPECT_EQ(mid[0], 255u) << "Mid R";
  EXPECT_EQ(mid[2], 255u) << "Mid B";

  // Bottom strip: green.
  auto bottom = pixelAt(pixels, 8, 56);
  EXPECT_EQ(bottom[0], 0u) << "Bottom R";
  EXPECT_EQ(bottom[1], 255u) << "Bottom G";
}

/// Fill a rectangle sampling a 4x4 solid-red pattern tile. Verifies the
/// pattern-sampling fragment path end-to-end: tile texture upload, Slug
/// winding coverage, and `fract()`-based wrap.
TEST_F(GeoEncoderTest, FillPathPatternSolidTile) {
  // 1. Build a 4x4 solid red RGBA8 pattern tile (premultiplied).
  constexpr uint32_t kTileDim = 4;
  std::array<uint8_t, kTileDim * kTileDim * 4> tilePixels;
  for (size_t i = 0; i < tilePixels.size(); i += 4) {
    tilePixels[i + 0] = 255;  // R
    tilePixels[i + 1] = 0;
    tilePixels[i + 2] = 0;
    tilePixels[i + 3] = 255;
  }
  wgpu::TextureDescriptor td = {};
  td.label = "PatternTile";
  td.size = {kTileDim, kTileDim, 1};
  td.format = kFormat;
  td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture tile = device_->device().CreateTexture(&td);
  ASSERT_TRUE(static_cast<bool>(tile));

  wgpu::TexelCopyTextureInfo dst = {};
  dst.texture = tile;
  wgpu::TexelCopyBufferLayout layout = {};
  layout.bytesPerRow = kTileDim * 4;
  layout.rowsPerImage = kTileDim;
  wgpu::Extent3D extent = {kTileDim, kTileDim, 1};
  device_->queue().WriteTexture(&dst, tilePixels.data(), tilePixels.size(), &layout, &extent);

  // 2. Fill a path with the pattern. The tile size in pattern-space is
  // 4x4 so the shader wraps every 4 pixels; since the path spans more than
  // one tile, the wrap logic is exercised.
  Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, msaaTarget_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  GeoEncoder::PatternPaint paint;
  paint.tile = tile;
  paint.tileSize = Vector2d(4.0, 4.0);
  paint.patternFromPath = Transform2d();  // Identity: target space == pattern space.
  paint.opacity = 1.0;
  encoder.fillPathPattern(path, FillRule::NonZero, paint);
  encoder.finish();

  auto pixels = readback();
  auto center = pixelAt(pixels, 32, 32);
  EXPECT_EQ(center[0], 255u) << "Center R (pattern sampled)";
  EXPECT_EQ(center[1], 0u) << "Center G";
  EXPECT_EQ(center[2], 0u) << "Center B";
  EXPECT_EQ(center[3], 255u) << "Center A";

  // Outside the rect should stay at the clear color (black).
  auto corner = pixelAt(pixels, 4, 4);
  EXPECT_EQ(corner[0], 0u) << "Corner R";
  EXPECT_EQ(corner[3], 255u) << "Corner A (opaque clear)";
}

}  // namespace

}  // namespace donner::geode
