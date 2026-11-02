/// @file
/// GeodeSurfaceRenderer over real Core Animation layers: creation, the layer configuration it
/// promises, presentation, resize and resize-needed handling, and argument validation.
///
/// These cases cannot prove that pixels reached the layer (a capture of a borrowed drawable is
/// refused by design), device-loss recovery, the destructor's bounded wait, or threading.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/GeodeSurfaceRenderer.h"

namespace donner::svg {
namespace {

using testing::Eq;
using testing::HasSubstr;
using testing::IsNull;
using testing::NotNull;
using FrameStatus = GeodeSurfaceRenderer::FrameStatus;

/// A drawable can be briefly unavailable; this bounds the retries before a frame must present.
constexpr int kMaxPresentAttempts = 32;

constexpr std::string_view kDocument =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
         <rect width="64" height="64" fill="#d33"/>
         <circle cx="32" cy="32" r="16" fill="#3d3"/>
       </svg>)";

/// Draws until a frame is not skipped or the attempt budget runs out, returning the last status.
FrameStatus DrawUntilNotSkipped(GeodeSurfaceRenderer& renderer, SVGDocument& document) {
  FrameStatus status = FrameStatus::Skipped;
  for (int attempt = 0; attempt < kMaxPresentAttempts && status == FrameStatus::Skipped;
       ++attempt) {
    status = renderer.draw(document);
  }
  return status;
}

class GeodeSurfaceRendererTest : public testing::Test {
protected:
  void SetUp() override {
    layer_ = [CAMetalLayer layer];
    layer_.contentsScale = 1.0;

    ParseWarningSink warnings;
    ParseResult<SVGDocument> parsed = parser::SVGParser::ParseSVG(kDocument, warnings);
    ASSERT_FALSE(parsed.hasError()) << parsed.error();
    document_ = std::move(parsed.result());
  }

  void* layer() { return (__bridge void*)layer_; }

  /// Creates a renderer over the test layer, failing the test when creation fails.
  std::unique_ptr<GeodeSurfaceRenderer> create(Vector2i sizePx) {
    std::string error;
    std::unique_ptr<GeodeSurfaceRenderer> renderer =
        GeodeSurfaceRenderer::CreateForMetalLayer(layer(), sizePx, &error);
    EXPECT_THAT(renderer, NotNull()) << error;
    return renderer;
  }

  CAMetalLayer* layer_ = nil;
  std::optional<SVGDocument> document_;
};

TEST_F(GeodeSurfaceRendererTest, ConfiguresTheLayerItPresentsTo) {
  std::unique_ptr<GeodeSurfaceRenderer> renderer = create(Vector2i(64, 64));
  ASSERT_THAT(renderer, NotNull());

  EXPECT_THAT(renderer->size(), Eq(Vector2i(64, 64)));
  EXPECT_THAT(layer_.device, NotNull());
  EXPECT_THAT(layer_.pixelFormat, Eq(MTLPixelFormatBGRA8Unorm));
  EXPECT_THAT(layer_.drawableSize.width, Eq(64.0));
  EXPECT_THAT(layer_.drawableSize.height, Eq(64.0));
  EXPECT_TRUE(layer_.opaque) << "the documented opaque configuration";
}

TEST_F(GeodeSurfaceRendererTest, PresentsADocument) {
  std::unique_ptr<GeodeSurfaceRenderer> renderer = create(Vector2i(64, 64));
  ASSERT_THAT(renderer, NotNull());

  document_->setCanvasSize(64, 64);
  EXPECT_THAT(DrawUntilNotSkipped(*renderer, *document_), Eq(FrameStatus::Presented));
  // Presenting again reuses the surface and device.
  EXPECT_THAT(DrawUntilNotSkipped(*renderer, *document_), Eq(FrameStatus::Presented));
  EXPECT_FALSE(renderer->isDeviceLost());
}

TEST_F(GeodeSurfaceRendererTest, ResizeReconfiguresTheLayer) {
  std::unique_ptr<GeodeSurfaceRenderer> renderer = create(Vector2i(64, 64));
  ASSERT_THAT(renderer, NotNull());

  std::string error;
  ASSERT_TRUE(renderer->resize(Vector2i(96, 48), &error)) << error;
  EXPECT_THAT(renderer->size(), Eq(Vector2i(96, 48)));
  EXPECT_THAT(layer_.drawableSize.width, Eq(96.0));
  EXPECT_THAT(layer_.drawableSize.height, Eq(48.0));
  EXPECT_TRUE(renderer->resize(Vector2i(96, 48))) << "resizing to the current size is a no-op";

  document_->setCanvasSize(96, 48);
  EXPECT_THAT(DrawUntilNotSkipped(*renderer, *document_), Eq(FrameStatus::Presented));

  EXPECT_FALSE(renderer->resize(Vector2i(0, 48), &error));
  EXPECT_THAT(error, HasSubstr("positive"));
  EXPECT_THAT(renderer->size(), Eq(Vector2i(96, 48))) << "the previous size stays in effect";
}

/// When the laid-out layer's pixel extent drifts from the configured size, draw reports it
/// instead of presenting a stretched frame, and a resize to the new extent recovers.
TEST_F(GeodeSurfaceRendererTest, ReportsAResizeWhenTheLayerExtentChanges) {
  layer_.bounds = CGRectMake(0, 0, 32, 32);
  layer_.contentsScale = 2.0;
  std::unique_ptr<GeodeSurfaceRenderer> renderer = create(Vector2i(64, 64));
  ASSERT_THAT(renderer, NotNull());
  document_->setCanvasSize(64, 64);
  ASSERT_THAT(DrawUntilNotSkipped(*renderer, *document_), Eq(FrameStatus::Presented));

  layer_.bounds = CGRectMake(0, 0, 50, 40);
  EXPECT_THAT(DrawUntilNotSkipped(*renderer, *document_), Eq(FrameStatus::ResizeNeeded));
  EXPECT_THAT(renderer->draw(*document_), Eq(FrameStatus::ResizeNeeded))
      << "the status persists until the host resizes";

  ASSERT_TRUE(renderer->resize(Vector2i(100, 80)));
  document_->setCanvasSize(100, 80);
  EXPECT_THAT(DrawUntilNotSkipped(*renderer, *document_), Eq(FrameStatus::Presented));
}

TEST_F(GeodeSurfaceRendererTest, RejectsALayerThatIsNotAMetalLayer) {
  CALayer* plainLayer = [CALayer layer];
  std::string error;
  EXPECT_THAT(GeodeSurfaceRenderer::CreateForMetalLayer((__bridge void*)plainLayer,
                                                        Vector2i(64, 64), &error),
              IsNull());
  EXPECT_THAT(error, HasSubstr("could not create a surface"));
}

TEST_F(GeodeSurfaceRendererTest, RejectsANullLayer) {
  std::string error;
  EXPECT_THAT(GeodeSurfaceRenderer::CreateForMetalLayer(nullptr, Vector2i(64, 64), &error),
              IsNull());
  EXPECT_THAT(error, HasSubstr("null"));
}

TEST_F(GeodeSurfaceRendererTest, RejectsAnEmptySize) {
  std::string error;
  EXPECT_THAT(GeodeSurfaceRenderer::CreateForMetalLayer(layer(), Vector2i(64, 0), &error),
              IsNull());
  EXPECT_THAT(error, HasSubstr("positive"));
  // The error out-parameter is optional.
  EXPECT_THAT(GeodeSurfaceRenderer::CreateForMetalLayer(layer(), Vector2i(-1, 64)), IsNull());
}

/// A frame target larger than the renderer's surface budget would never be drawn, so it is
/// refused up front rather than presented undrawn.
TEST_F(GeodeSurfaceRendererTest, RejectsASizeBeyondTheSurfaceBudget) {
  std::string error;
  EXPECT_THAT(GeodeSurfaceRenderer::CreateForMetalLayer(layer(), Vector2i(16384, 16384), &error),
              IsNull());
  EXPECT_THAT(error, HasSubstr("surface budget"));

  std::unique_ptr<GeodeSurfaceRenderer> renderer = create(Vector2i(64, 64));
  ASSERT_THAT(renderer, NotNull());
  EXPECT_FALSE(renderer->resize(Vector2i(16384, 16384), &error));
  EXPECT_THAT(error, HasSubstr("surface budget"));
  EXPECT_THAT(renderer->size(), Eq(Vector2i(64, 64)));
}

/// Fractional point bounds round to the nearest pixel on each axis, which is the extent the
/// layer reports; a host that resizes to that rounded extent presents again.
TEST_F(GeodeSurfaceRendererTest, AcceptsTheRoundedExtentOfFractionalBounds) {
  layer_.bounds = CGRectMake(0, 0, 32.25, 20.75);
  layer_.contentsScale = 2.0;  // 64.5 x 41.5 rounds to 65 x 42.
  std::unique_ptr<GeodeSurfaceRenderer> renderer = create(Vector2i(65, 42));
  ASSERT_THAT(renderer, NotNull());
  document_->setCanvasSize(65, 42);
  EXPECT_THAT(DrawUntilNotSkipped(*renderer, *document_), Eq(FrameStatus::Presented));
}

}  // namespace
}  // namespace donner::svg
