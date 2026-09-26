#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>

#include "donner/svg/resources/FontCatalog.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextBackendFull.h"

namespace donner::svg {
namespace {

Path OutlineFor(TextBackendFull& backend, FontHandle face) {
  const auto shaped = backend.shapeRun(face, 64.0f, "A", 0, 1, false, FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs, ::testing::SizeIs(1));
  if (shaped.glyphs.empty()) {
    return {};
  }
  return backend.glyphOutline(face, shaped.glyphs.front().glyphIndex,
                              backend.scaleForEmToPixels(face, 64.0f));
}

TEST(FontCatalogVariableAxesTest, InterWeightAndItalicChangeGlyphOutlines) {
  FontCatalog catalog;
  Registry registry;
  FontManager manager(registry);
  manager.setFontProvider(&catalog);
  TextBackendFull backend(manager, registry);

  const FontHandle regular = manager.findFont("Inter", 400);
  const FontHandle bold = manager.findFont("Inter", 700);
  const FontHandle italic = manager.findFont("Inter", 400, static_cast<int>(FontStyle::Italic), 5);
  const FontHandle boldItalic =
      manager.findFont("Inter", 700, static_cast<int>(FontStyle::Italic), 5);
  ASSERT_TRUE(regular);
  ASSERT_TRUE(bold);
  ASSERT_TRUE(italic);
  ASSERT_TRUE(boldItalic);

  const Path regularOutline = OutlineFor(backend, regular);
  const Path boldOutline = OutlineFor(backend, bold);
  const Path italicOutline = OutlineFor(backend, italic);
  const Path boldItalicOutline = OutlineFor(backend, boldItalic);
  ASSERT_FALSE(regularOutline.empty());
  ASSERT_FALSE(boldOutline.empty());
  ASSERT_FALSE(italicOutline.empty());
  ASSERT_FALSE(boldItalicOutline.empty());
  EXPECT_THAT(boldOutline.points(),
              ::testing::Not(::testing::ElementsAreArray(regularOutline.points())));
  EXPECT_THAT(italicOutline.points(),
              ::testing::Not(::testing::ElementsAreArray(regularOutline.points())));
  EXPECT_THAT(boldItalicOutline.points(),
              ::testing::Not(::testing::ElementsAreArray(boldOutline.points())));
}

TEST(FontCatalogVariableAxesTest, StaticCatalogFaceSynthesizesMissingBoldAndItalic) {
  FontCatalog catalog;
  Registry registry;
  FontManager manager(registry);
  manager.setFontProvider(&catalog);
  TextBackendFull backend(manager, registry);

  const FontHandle regular = manager.findFont("Lato", 400);
  const FontHandle bold = manager.findFont("Lato", 700);
  const FontHandle italic = manager.findFont("Lato", 400, static_cast<int>(FontStyle::Italic), 5);
  ASSERT_TRUE(regular);
  ASSERT_TRUE(bold);
  ASSERT_TRUE(italic);
  const Path regularOutline = OutlineFor(backend, regular);
  const Path boldOutline = OutlineFor(backend, bold);
  const Path italicOutline = OutlineFor(backend, italic);
  ASSERT_FALSE(regularOutline.empty());
  ASSERT_FALSE(boldOutline.empty());
  ASSERT_FALSE(italicOutline.empty());
  EXPECT_THAT(boldOutline.points(),
              ::testing::Not(::testing::ElementsAreArray(regularOutline.points())));
  EXPECT_THAT(italicOutline.points(),
              ::testing::Not(::testing::ElementsAreArray(regularOutline.points())));
}

TEST(FontCatalogVariableAxesTest, IntrinsicGenericFacesAreNotSynthesizedTwice) {
  FontCatalog catalog;
  Registry registry;
  FontManager manager(registry);
  manager.setFontProvider(&catalog);
  TextBackendFull backend(manager, registry);

  for (const auto [weight, style] :
       {std::pair{700, FontStyle::Normal}, std::pair{400, FontStyle::Italic}}) {
    const FontHandle catalogFace =
        manager.findFont("sans-serif", weight, static_cast<int>(style), 5);
    ASSERT_TRUE(catalogFace);
    const FontHandle rawFace =
        manager.loadFontData(manager.fontData(catalogFace), FontDataTrust::Trusted);
    ASSERT_TRUE(rawFace);
    const Path catalogOutline = OutlineFor(backend, catalogFace);
    const Path rawOutline = OutlineFor(backend, rawFace);
    ASSERT_FALSE(catalogOutline.empty());
    EXPECT_THAT(catalogOutline.points(), ::testing::ElementsAreArray(rawOutline.points()));
  }
}

}  // namespace
}  // namespace donner::svg
