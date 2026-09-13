/// @file
/// Deferred font readiness must change rendered geometry without editing the document.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/EditorApp.h"
#include "donner/editor/TextToOutlines.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/resources/EmbeddedFontProvider.h"
#include "donner/svg/resources/FontCatalog.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::editor {
namespace {

using testing::Each;
using testing::Field;
using testing::Gt;
using testing::IsEmpty;
using testing::Not;
using testing::SizeIs;

constexpr std::string_view kTwoRoots = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="360" height="100">
  <text id="first" x="5" y="25" font-family="Inter" font-size="24">mmmm WiWi</text>
  <text id="second" x="5" y="55" font-family="Inter" font-size="24">mmmm WiWi</text>
  <text id="unchanged" x="5" y="90" font-family="sans-serif">Other text</text>
</svg>)svg";

void AttachProvider(svg::SVGDocument& document, const svg::FontFamilyProvider& provider) {
  const auto access = document.writeAccess();
  auto& registry = document.registry();
  if (!registry.ctx().contains<svg::FontManager>()) {
    registry.ctx().emplace<svg::FontManager>(registry);
  }
  registry.ctx().get<svg::FontManager>().setFontProvider(&provider);
}

bool StageInter(svg::CatalogEncodedFontStore& store, const svg::FontCatalog& catalog) {
  const auto id = catalog.availability("Inter", {}).contentId;
  if (!store.queue(id)) return false;
  const auto token = store.beginFetch(id);
  if (token == 0) return false;
  return store.publishVerified(id, token, svg::EmbeddedFontProvider().loadFamilyData("Inter", {}));
}

void ExpectReferencedFontRefresh(std::string_view source, std::string_view evidenceName) {
  auto store = std::make_shared<svg::CatalogEncodedFontStore>();
  svg::FontCatalog catalog(store);
  EditorApp app;
  ASSERT_EQ(app.loadFromString(source), true);
  auto& document = app.document().document();
  AttachProvider(document, catalog);
  const auto target = *document.querySelector("#target");
  const auto preflight = document.preflightFontResourcesForElement(target);
  EXPECT_EQ(preflight.status, svg::FontResourcePreflight::Status::PendingFonts);
  ASSERT_THAT(preflight.dependencies, SizeIs(1));
  EXPECT_EQ(preflight.dependencies[0].family, "inter");

  svg::Renderer renderer;
  renderer.draw(document);
  const auto sourceBefore = std::string(document.source());
  ASSERT_EQ(StageInter(*store, catalog), true);
  ASSERT_EQ(store->adoptReadyAssets(), true);
  ASSERT_EQ(app.document().refreshFontResources(), true);
  EXPECT_EQ(document.hasPendingRenderInvalidation(), true);
  renderer.draw(document);
  const auto actual = renderer.takeSnapshot();
  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_EQ(app.canUndo(), false);

  svg::FontCatalog embedded;
  EditorApp control;
  ASSERT_EQ(control.loadFromString(source), true);
  AttachProvider(control.document().document(), embedded);
  svg::Renderer controlRenderer;
  controlRenderer.draw(control.document().document());
  tests::CompareBitmapToBitmap(actual, controlRenderer.takeSnapshot(), std::string(evidenceName),
                               tests::PixelmatchIdentityParams());
}

TEST(CatalogFontResourcesTest, FontReadyReplacesFallbackWithoutSourceMutation) {
  auto store = std::make_shared<svg::CatalogEncodedFontStore>();
  svg::FontCatalog catalog(store);
  EditorApp app;
  ASSERT_EQ(app.loadFromString(kTwoRoots), true);
  auto& document = app.document().document();
  AttachProvider(document, catalog);
  svg::Renderer renderer;
  renderer.draw(document);
  auto first = document.querySelector("#first")->cast<svg::SVGTextElement>();
  auto second = document.querySelector("#second")->cast<svg::SVGTextElement>();
  const double fallbackLength = first.getComputedTextLength();
  EXPECT_DOUBLE_EQ(second.getComputedTextLength(), fallbackLength);
  ASSERT_THAT(document.fontDependenciesForElement(first), SizeIs(1));
  ASSERT_THAT(document.fontDependenciesForElement(second), SizeIs(1));
  const std::string sourceBefore(document.source());
  const auto frameBefore = app.document().currentFrameVersion();
  const auto generationBefore = app.document().documentGeneration();
  const auto unchanged = document.querySelector("#unchanged")->cast<svg::SVGTextElement>();
  const double unchangedLength = unchanged.getComputedTextLength();

  ASSERT_EQ(StageInter(*store, catalog), true);
  EXPECT_EQ(app.document().refreshFontResources(), false);
  EXPECT_DOUBLE_EQ(first.getComputedTextLength(), fallbackLength);
  ASSERT_EQ(store->adoptReadyAssets(), true);
  ASSERT_EQ(app.document().refreshFontResources(), true);

  EXPECT_EQ(app.document().currentFrameVersion(), frameBefore + 1);
  EXPECT_EQ(app.document().fontResourceRevision(), 1u);
  EXPECT_EQ(app.document().documentGeneration(), generationBefore);
  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_EQ(app.canUndo(), false);
  EXPECT_DOUBLE_EQ(unchanged.getComputedTextLength(), unchangedLength);
  EXPECT_THAT(first.getComputedTextLength(), Not(testing::DoubleEq(fallbackLength)));
  EXPECT_DOUBLE_EQ(second.getComputedTextLength(), first.getComputedTextLength());
  EXPECT_EQ(app.document().refreshFontResources(), false);
  renderer.draw(document);
  const auto actual = renderer.takeSnapshot();

  svg::FontCatalog embeddedCatalog;
  EditorApp reference;
  ASSERT_EQ(reference.loadFromString(kTwoRoots), true);
  AttachProvider(reference.document().document(), embeddedCatalog);
  svg::Renderer referenceRenderer;
  referenceRenderer.draw(reference.document().document());
  tests::CompareBitmapToBitmap(actual, referenceRenderer.takeSnapshot(), "catalog_font_adoption",
                               tests::PixelmatchIdentityParams());
}

TEST(CatalogFontResourcesTest, FontCompletionAfterDocumentSwitchIsIgnored) {
  auto store = std::make_shared<svg::CatalogEncodedFontStore>();
  svg::FontCatalog catalog(store);
  EditorApp app;
  ASSERT_EQ(app.loadFromString(kTwoRoots), true);
  AttachProvider(app.document().document(), catalog);
  const auto first = *app.document().document().querySelector("#first");
  ASSERT_THAT(app.document().document().preflightFontResourcesForElement(first).dependencies,
              Not(IsEmpty()));
  ASSERT_EQ(StageInter(*store, catalog), true);

  ASSERT_EQ(app.loadFromString(
                "<svg xmlns='http://www.w3.org/2000/svg'><rect width='10' height='10'/></svg>"),
            true);
  const std::string replacement(app.document().document().source());
  const auto frame = app.document().currentFrameVersion();
  ASSERT_EQ(store->adoptReadyAssets(), true);
  EXPECT_EQ(app.document().refreshFontResources(), false);
  EXPECT_EQ(app.document().currentFrameVersion(), frame);
  EXPECT_EQ(app.document().document().source(), replacement);
  EXPECT_THAT(app.document().document().renderedFontDependencies(), IsEmpty());
  EXPECT_EQ(app.canUndo(), false);
}

TEST(CatalogFontResourcesTest, OffscreenOutputDiscoversFontAndRefusesFallbackGeometry) {
  auto store = std::make_shared<svg::CatalogEncodedFontStore>();
  svg::FontCatalog catalog(store);
  EditorApp app;
  ASSERT_EQ(
      app.loadFromString(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
    <text id="offscreen" x="10000" y="20" font-family="Inter">Deferred output</text>
  </svg>)svg"),
      true);
  auto& document = app.document().document();
  AttachProvider(document, catalog);
  const auto target = *document.querySelector("#offscreen");
  const std::string original(document.source());
  const auto preflight = document.preflightFontResourcesForElement(target);
  EXPECT_EQ(preflight.status, svg::FontResourcePreflight::Status::PendingFonts);
  const auto& dependencies = preflight.dependencies;
  ASSERT_THAT(dependencies, SizeIs(1));
  EXPECT_THAT(dependencies, Each(Field(&svg::FontFaceDependency::state,
                                       svg::FontFaceLoadState::WaitingForBytes)));
  const auto pending = convertTextToOutlines(document, target);
  EXPECT_EQ(pending.ok, false);
  EXPECT_THAT(pending.error, testing::HasSubstr("font is not ready"));
  EXPECT_EQ(document.source(), original);
  ASSERT_EQ(StageInter(*store, catalog), true);
  ASSERT_EQ(store->adoptReadyAssets(), true);
  ASSERT_EQ(app.document().refreshFontResources(), true);
  const auto ready = convertTextToOutlines(document, target);
  EXPECT_EQ(ready.ok, true) << ready.error;
  EXPECT_THAT(ready.outlinePaths, Not(IsEmpty()));
}

TEST(CatalogFontResourcesTest, FontRelativeShapeWithoutTextRefreshesMetrics) {
  constexpr std::string_view source =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="180" height="80">
    <rect id="target" x="10" y="10" width="10ch" height="30" font-family="Inter" font-size="24"/>
  </svg>)svg";
  auto store = std::make_shared<svg::CatalogEncodedFontStore>();
  svg::FontCatalog catalog(store);
  EditorApp app;
  ASSERT_EQ(app.loadFromString(source), true);
  auto& document = app.document().document();
  AttachProvider(document, catalog);
  svg::Renderer renderer;
  renderer.draw(document);
  const auto shape = document.querySelector("#target")->cast<svg::SVGGeometryElement>();
  const auto before = shape.worldBounds();
  ASSERT_EQ(before.has_value(), true);
  ASSERT_THAT(document.renderedFontDependencies(), SizeIs(1));
  ASSERT_EQ(StageInter(*store, catalog), true);
  ASSERT_EQ(store->adoptReadyAssets(), true);
  ASSERT_EQ(app.document().refreshFontResources(), true);
  renderer.draw(document);
  const auto after = shape.worldBounds();
  ASSERT_EQ(after.has_value(), true);
  EXPECT_THAT(after->width(), Not(testing::DoubleEq(before->width())));
  ExpectReferencedFontRefresh(source, "catalog_ch_geometry");
}

TEST(CatalogFontResourcesTest, ReferencedTextClipExcludesUnusedDefinitions) {
  ExpectReferencedFontRefresh(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">
    <defs>
      <clipPath id="clip"><text x="8" y="58" font-size="60" font-family="Inter">M</text></clipPath>
      <text id="unused" font-family="Lora">Never consumed</text>
    </defs>
    <rect id="target" width="100" height="80" clip-path="url(#clip)" fill="red"/>
  </svg>)svg",
                              "catalog_text_clip");
}

TEST(CatalogFontResourcesTest, SharedNestedPatternPropagatesFontReadinessToItsConsumer) {
  ExpectReferencedFontRefresh(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="160" height="80">
    <defs>
      <pattern id="letters" width="40" height="80" patternUnits="userSpaceOnUse">
        <text x="2" y="52" font-size="42" font-family="Inter">M</text>
      </pattern>
      <pattern id="outer" width="80" height="80" patternUnits="userSpaceOnUse">
        <rect width="80" height="80" fill="url(#letters)"/>
      </pattern>
      <pattern id="unused" width="20" height="20"><text font-family="Lora">Unused</text></pattern>
    </defs>
    <g id="target"><rect width="80" height="80" fill="url(#outer)"/>
      <rect x="80" width="80" height="80" fill="url(#outer)"/></g>
  </svg>)svg",
                              "catalog_nested_pattern");
}

TEST(CatalogFontResourcesTest, ForeignAndDetachedTargetsFailClosed) {
  EditorApp app;
  EditorApp other;
  ASSERT_EQ(app.loadFromString(kTwoRoots), true);
  ASSERT_EQ(other.loadFromString(kTwoRoots), true);
  auto& document = app.document().document();
  const auto foreign = *other.document().document().querySelector("#first");
  EXPECT_EQ(document.preflightFontResourcesForElement(foreign).status,
            svg::FontResourcePreflight::Status::InvalidTarget);
  const auto conversion = convertTextToOutlines(document, foreign);
  EXPECT_EQ(conversion.ok, false);
  EXPECT_THAT(conversion.error, testing::HasSubstr("not attached to this document"));
  auto detached = *document.querySelector("#first");
  detached.remove();
  EXPECT_EQ(document.preflightFontResourcesForElement(detached).status,
            svg::FontResourcePreflight::Status::InvalidTarget);
}

TEST(CatalogFontResourcesTest, PreflightPreservesPendingRenderInvalidation) {
  EditorApp app;
  ASSERT_EQ(app.loadFromString(kTwoRoots), true);
  auto& document = app.document().document();
  svg::Renderer renderer;
  renderer.draw(document);
  auto target = *document.querySelector("#first");
  target.setAttribute("font-size", "36");
  ASSERT_EQ(document.hasPendingRenderInvalidation(), true);
  const auto sourceBefore = std::string(document.source());
  EXPECT_EQ(document.preflightFontResourcesForElement(target).status,
            svg::FontResourcePreflight::Status::NeedsRender);
  EXPECT_EQ(document.hasPendingRenderInvalidation(), true);
  EXPECT_EQ(document.source(), sourceBefore);
}

TEST(CatalogFontResourcesTest, DependencyOverflowCannotPassOutputPreflight) {
  auto store = std::make_shared<svg::CatalogEncodedFontStore>();
  svg::FontCatalog catalog(store);
  EditorApp app;
  ASSERT_EQ(app.loadFromString(kTwoRoots), true);
  auto& document = app.document().document();
  auto& registry = document.registry();
  auto& manager = registry.ctx().emplace<svg::FontManager>(
      registry, svg::FontManager::kDefaultMaximumLoadedFontBytes, 0);
  manager.setFontProvider(&catalog);
  const auto target = *document.querySelector("#first");
  EXPECT_EQ(document.preflightFontResourcesForElement(target).status,
            svg::FontResourcePreflight::Status::ResourceLimit);
  EXPECT_EQ(document.fontResourcesExceeded(), true);
  EXPECT_EQ(convertTextToOutlines(document, target).ok, false);
}

}  // namespace
}  // namespace donner::editor
