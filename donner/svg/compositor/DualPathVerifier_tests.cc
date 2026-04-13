#include "donner/svg/compositor/DualPathVerifier.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/tests/ParserTestUtils.h"

using ::testing::_;
using ::testing::AtLeast;
using ::testing::NiceMock;
using ::testing::Return;

namespace donner::svg::compositor {

namespace {

class MockRendererInterface : public RendererInterface {
public:
  MOCK_METHOD(void, draw, (SVGDocument & document), (override));
  MOCK_METHOD(int, width, (), (const, override));
  MOCK_METHOD(int, height, (), (const, override));
  MOCK_METHOD(void, beginFrame, (const RenderViewport& viewport), (override));
  MOCK_METHOD(void, endFrame, (), (override));
  MOCK_METHOD(void, setTransform, (const Transform2d& transform), (override));
  MOCK_METHOD(void, pushTransform, (const Transform2d& transform), (override));
  MOCK_METHOD(void, popTransform, (), (override));
  MOCK_METHOD(void, pushClip, (const ResolvedClip& clip), (override));
  MOCK_METHOD(void, popClip, (), (override));
  MOCK_METHOD(void, pushIsolatedLayer, (double opacity, MixBlendMode blendMode), (override));
  MOCK_METHOD(void, popIsolatedLayer, (), (override));
  MOCK_METHOD(void, pushFilterLayer,
              (const components::FilterGraph& filterGraph,
               const std::optional<Box2d>& filterRegion),
              (override));
  MOCK_METHOD(void, popFilterLayer, (), (override));
  MOCK_METHOD(void, pushMask, (const std::optional<Box2d>& maskBounds), (override));
  MOCK_METHOD(void, transitionMaskToContent, (), (override));
  MOCK_METHOD(void, popMask, (), (override));
  MOCK_METHOD(void, beginPatternTile, (const Box2d& tileRect, const Transform2d& targetFromPattern),
              (override));
  MOCK_METHOD(void, endPatternTile, (bool forStroke), (override));
  MOCK_METHOD(void, setPaint, (const PaintParams& paint), (override));
  MOCK_METHOD(void, drawPath, (const PathShape& path, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawRect, (const Box2d& rect, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawEllipse, (const Box2d& bounds, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawImage, (const ImageResource& image, const ImageParams& params), (override));
  MOCK_METHOD(void, drawText,
              (Registry & registry, const components::ComputedTextComponent& text,
               const TextParams& params),
              (override));
  MOCK_METHOD(RendererBitmap, takeSnapshot, (), (const, override));
  MOCK_METHOD(std::unique_ptr<RendererInterface>, createOffscreenInstance, (), (const, override));
};

RendererBitmap makeSolidBitmap(int width, int height, uint8_t r, uint8_t g, uint8_t b,
                               uint8_t a = 255) {
  RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(width, height);
  bitmap.rowBytes = static_cast<size_t>(width) * 4;
  bitmap.pixels.resize(bitmap.rowBytes * height);
  for (size_t i = 0; i < bitmap.pixels.size(); i += 4) {
    bitmap.pixels[i] = r;
    bitmap.pixels[i + 1] = g;
    bitmap.pixels[i + 2] = b;
    bitmap.pixels[i + 3] = a;
  }
  return bitmap;
}

}  // namespace

class DualPathVerifierTest : public ::testing::Test {
protected:
  SVGDocument makeDocument(std::string_view svg, Vector2i size = kTestSvgDefaultSize) {
    return instantiateSubtree(svg, parser::SVGParser::Options(), size);
  }

  NiceMock<MockRendererInterface> renderer_;
};

TEST_F(DualPathVerifierTest, IdenticalOutputsPassVerification) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="10" height="10" fill="red" />
  )svg");

  // Both calls to takeSnapshot return the same bitmap.
  const RendererBitmap solidRed = makeSolidBitmap(16, 16, 255, 0, 0);
  EXPECT_CALL(renderer_, takeSnapshot()).WillRepeatedly(Return(solidRed));

  CompositorController compositor(document, renderer_);
  DualPathVerifier verifier(compositor, renderer_);

  RenderViewport viewport;
  viewport.size = Vector2d(16, 16);
  viewport.devicePixelRatio = 1.0;

  auto result = verifier.renderAndVerify(viewport);
  EXPECT_TRUE(result.isExact()) << result;
  EXPECT_EQ(result.mismatchCount, 0u);
  EXPECT_EQ(result.maxChannelDiff, 0);
  EXPECT_TRUE(result.dimensionsMatch);
}

TEST_F(DualPathVerifierTest, DifferentOutputsDetected) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="10" height="10" fill="red" />
  )svg");

  // First snapshot (compositor): solid red. Second snapshot (reference): solid blue.
  const RendererBitmap solidRed = makeSolidBitmap(16, 16, 255, 0, 0);
  const RendererBitmap solidBlue = makeSolidBitmap(16, 16, 0, 0, 255);
  EXPECT_CALL(renderer_, takeSnapshot())
      .WillOnce(Return(solidRed))
      .WillOnce(Return(solidBlue));

  CompositorController compositor(document, renderer_);
  DualPathVerifier verifier(compositor, renderer_);

  RenderViewport viewport;
  viewport.size = Vector2d(16, 16);
  viewport.devicePixelRatio = 1.0;

  auto result = verifier.renderAndVerify(viewport);
  EXPECT_FALSE(result.isExact()) << result;
  EXPECT_GT(result.mismatchCount, 0u);
  EXPECT_EQ(result.maxChannelDiff, 255);  // Red vs blue.
  EXPECT_EQ(result.totalPixels, 256u);    // 16x16.
}

TEST_F(DualPathVerifierTest, ToleranceCheck) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="10" height="10" fill="red" />
  )svg");

  // Bitmaps differ by 1 LSB in the red channel.
  const RendererBitmap bitmapA = makeSolidBitmap(4, 4, 200, 100, 50);
  const RendererBitmap bitmapB = makeSolidBitmap(4, 4, 201, 100, 50);
  EXPECT_CALL(renderer_, takeSnapshot())
      .WillOnce(Return(bitmapA))
      .WillOnce(Return(bitmapB));

  CompositorController compositor(document, renderer_);
  DualPathVerifier verifier(compositor, renderer_);

  RenderViewport viewport;
  viewport.size = Vector2d(4, 4);
  viewport.devicePixelRatio = 1.0;

  auto result = verifier.renderAndVerify(viewport);
  EXPECT_FALSE(result.isExact());
  EXPECT_TRUE(result.isWithinTolerance(1)) << result;
  EXPECT_FALSE(result.isWithinTolerance(0));
  EXPECT_EQ(result.maxChannelDiff, 1);
}

TEST_F(DualPathVerifierTest, DimensionMismatchDetected) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="10" height="10" fill="red" />
  )svg");

  const RendererBitmap small = makeSolidBitmap(4, 4, 255, 0, 0);
  const RendererBitmap large = makeSolidBitmap(8, 8, 255, 0, 0);
  EXPECT_CALL(renderer_, takeSnapshot())
      .WillOnce(Return(small))
      .WillOnce(Return(large));

  CompositorController compositor(document, renderer_);
  DualPathVerifier verifier(compositor, renderer_);

  RenderViewport viewport;
  viewport.size = Vector2d(8, 8);
  viewport.devicePixelRatio = 1.0;

  auto result = verifier.renderAndVerify(viewport);
  EXPECT_FALSE(result.dimensionsMatch);
  EXPECT_FALSE(result.isExact());
}

TEST_F(DualPathVerifierTest, VerifyResultOstream) {
  DualPathVerifier::VerifyResult result;
  result.mismatchCount = 42;
  result.maxChannelDiff = 3;
  result.totalPixels = 1000;
  result.dimensionsMatch = true;

  std::ostringstream os;
  os << result;
  EXPECT_THAT(os.str(), ::testing::HasSubstr("42"));
  EXPECT_THAT(os.str(), ::testing::HasSubstr("1000"));
}

}  // namespace donner::svg::compositor
