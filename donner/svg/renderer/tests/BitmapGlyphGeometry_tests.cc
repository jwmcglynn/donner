#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "donner/base/MathUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

SVGDocument BitmapDocument(int angle, bool rotateParent) {
  const std::string rotation = std::to_string(angle);
  const std::string group = rotateParent ? "<g transform=\"rotate(" + rotation + " 96 128)\">" : "";
  const std::string textRotation = rotateParent ? "" : " rotate=\"" + rotation + "\"";
  SVGDocument document = instantiateSubtree(
      group +
          "<text id=\"emoji\" x=\"96\" y=\"128\" font-family=\"BitmapGeometryFixture\" "
          "font-size=\"32\" textLength=\"64\" lengthAdjust=\"spacingAndGlyphs\"" +
          textRotation + ">&#x1F601;</text>" + (rotateParent ? "</g>" : ""),
      {}, Vector2i(256, 256));
  const auto path = Runfiles::instance().Rlocation(
      "donner/svg/text/tests/font_decoder_corpus/bitmap_emoji_subset.ttf");
  std::ifstream source(path, std::ios::binary);
  auto bytes = std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(source),
                                                            std::istreambuf_iterator<char>());
  EXPECT_THAT(*bytes, testing::Not(testing::IsEmpty()));
  css::FontFace face;
  face.familyName = "BitmapGeometryFixture";
  css::FontFaceSource fontSource;
  fontSource.kind = css::FontFaceSource::Kind::Data;
  fontSource.payload = bytes;
  fontSource.trusted = true;
  face.sources.push_back(std::move(fontSource));
  auto& registry = document.registry();
  auto& manager = registry.ctx().contains<FontManager>()
                      ? registry.ctx().get<FontManager>()
                      : registry.ctx().emplace<FontManager>(registry);
  manager.addFontFace(face);
  return document;
}

Box2d IndependentlyRotatedBounds(const TextBackend::BitmapGlyphMetrics& metrics,
                                 const TextGlyph& glyph) {
  const double radians = glyph.rotateDegrees * MathConstants<double>::kPi / 180.0;
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);
  Vector2d minimum(std::numeric_limits<double>::infinity(),
                   std::numeric_limits<double>::infinity());
  Vector2d maximum(-minimum.x, -minimum.y);
  for (const double x :
       {metrics.bearingX, metrics.bearingX + metrics.width * metrics.scale * glyph.stretchScaleX}) {
    for (const double y : {-metrics.bearingY, -metrics.bearingY + metrics.height * metrics.scale *
                                                                      glyph.stretchScaleY}) {
      const Vector2d corner(glyph.xPosition + cosine * x - sine * y,
                            glyph.yPosition + sine * x + cosine * y);
      minimum.x = std::min(minimum.x, corner.x);
      minimum.y = std::min(minimum.y, corner.y);
      maximum.x = std::max(maximum.x, corner.x);
      maximum.y = std::max(maximum.y, corner.y);
    }
  }
  return Box2d(minimum, maximum);
}

auto BoundsNear(const Box2d& expected) {
  using testing::AllOf;
  using testing::DoubleNear;
  using testing::Field;
  return AllOf(Field("topLeft", &Box2d::topLeft,
                     AllOf(Field("x", &Vector2d::x, DoubleNear(expected.topLeft.x, 1e-8)),
                           Field("y", &Vector2d::y, DoubleNear(expected.topLeft.y, 1e-8)))),
               Field("bottomRight", &Box2d::bottomRight,
                     AllOf(Field("x", &Vector2d::x, DoubleNear(expected.bottomRight.x, 1e-8)),
                           Field("y", &Vector2d::y, DoubleNear(expected.bottomRight.y, 1e-8)))));
}

TEST(BitmapGlyphGeometryTest, RealFontPixelsBoundsHitAndCropMatchParentRotation) {
  for (const int angle : {90, -90, 30}) {
    SCOPED_TRACE(angle);
    SVGDocument actual = BitmapDocument(angle, false);
    SVGDocument reference = BitmapDocument(angle, true);
    Renderer actualRenderer;
    Renderer referenceRenderer;
    actualRenderer.draw(actual);
    referenceRenderer.draw(reference);
    const auto actualPixels = actualRenderer.takeSnapshot();
    const auto referencePixels = referenceRenderer.takeSnapshot();
    ASSERT_THAT(actualPixels.pixels, testing::Contains(testing::Gt(0)));
    ASSERT_THAT(referencePixels.pixels, testing::Contains(testing::Gt(0)));
    editor::tests::CompareBitmapToBitmap(actualPixels, referencePixels,
                                         "bitmap_geometry_full_" + std::to_string(angle),
                                         editor::tests::PixelmatchIdentityParams());

    const auto element = actual.querySelector("#emoji");
    const auto referenceElement = reference.querySelector("#emoji");
    ASSERT_THAT(element, testing::Ne(std::nullopt));
    ASSERT_THAT(referenceElement, testing::Ne(std::nullopt));
    const SVGTextElement text = element->cast<SVGTextElement>();
    auto& engine = actual.registry().ctx().get<TextEngine>();
    const auto& geometry = engine.ensureComputedTextGeometryComponent(element->entityHandle());
    ASSERT_THAT(geometry.runs, testing::SizeIs(1));
    ASSERT_THAT(geometry.runs[0].glyphs, testing::SizeIs(1));
    const auto& run = geometry.runs[0];
    const auto& glyph = run.glyphs[0];
    ASSERT_THAT(engine.isBitmapOnly(run.font), testing::IsTrue());
    EXPECT_THAT(glyph.stretchScaleX, testing::Gt(1.0f));
    const auto metrics = engine.bitmapGlyphMetrics(
        run.font, glyph.glyphIndex, engine.scaleForPixelHeight(run.font, run.usedFontSizePx));
    ASSERT_THAT(metrics, testing::Ne(std::nullopt));
    const Box2d expected = IndependentlyRotatedBounds(*metrics, glyph);
    EXPECT_THAT(text.getExtentOfChar(0), BoundsNear(expected));
    EXPECT_THAT(text.inkBoundingBox(), BoundsNear(expected));
    EXPECT_EQ(text.getCharNumAtPosition((expected.topLeft + expected.bottomRight) * 0.5), 0);
    EXPECT_EQ(text.getCharNumAtPosition(expected.bottomRight + Vector2d(2, 2)), -1);
    EXPECT_THAT(engine.computedGlyphPaths(element->entityHandle()), testing::IsEmpty());
    EXPECT_THAT(engine.computedGlyphOutlines(element->entityHandle()), testing::IsEmpty());

    const auto actualCrop = actualRenderer.renderElement(*element, Vector2i(80, 80)).bitmap();
    const auto referenceCrop =
        referenceRenderer.renderElement(*referenceElement, Vector2i(80, 80)).bitmap();
    ASSERT_THAT(actualCrop.empty(), testing::IsFalse());
    ASSERT_THAT(referenceCrop.empty(), testing::IsFalse());
    EXPECT_THAT(actualCrop.pixels, testing::Contains(testing::Gt(0)));
    editor::tests::CompareBitmapToBitmap(actualCrop, referenceCrop,
                                         "bitmap_geometry_crop_" + std::to_string(angle),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg
