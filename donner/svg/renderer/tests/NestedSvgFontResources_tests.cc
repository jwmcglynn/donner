/// @file
/// Separate SVG image registries must publish their font readiness through the painted host.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/FontPaintDependenciesComponent.h"
#include "donner/svg/components/FontResourceGraph.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
#include "donner/svg/resources/EmbeddedFontProvider.h"
#include "donner/svg/resources/FontCatalog.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

using testing::Each;
using testing::Field;
using testing::IsEmpty;
using testing::Not;
using testing::SizeIs;

class ScopedDefaultProvider {
public:
  explicit ScopedDefaultProvider(const FontFamilyProvider& provider)
      : previous_(FontManager::DefaultFontProvider()) {
    FontManager::SetDefaultFontProvider(&provider);
  }
  ~ScopedDefaultProvider() { FontManager::SetDefaultFontProvider(previous_); }

private:
  const FontFamilyProvider* previous_;
};

std::optional<SVGDocument> ParseDocument(std::string_view source) {
  auto warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  if (parsed.hasError()) {
    ADD_FAILURE() << parsed.error().reason;
    return std::nullopt;
  }
  return std::move(parsed.result());
}

std::string ChildUri() {
  constexpr std::string_view source =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="160" height="70"><text x="3" y="43" font-family="Inter" font-size="35">WiWmmm</text></svg>)svg";
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result = "data:image/svg+xml,";
  for (const unsigned char byte : source) {
    result.push_back('%');
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}

std::string ImageSource(bool twoHosts = false, bool parentText = false) {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="360" height="100">)";
  source += "<image id='first' width='160' height='70' href='" + ChildUri() + "'/>";
  if (twoHosts)
    source += "<image id='second' x='175' width='160' height='70' href='" + ChildUri() + "'/>";
  if (parentText) source += "<text id='parentText' y='95' font-family='Inter'>Parent</text>";
  source += "<rect id='unrelated' x='345' y='80' width='10' height='10'/></svg>";
  return source;
}

std::string FilterSource() {
  return R"(<svg xmlns="http://www.w3.org/2000/svg" width="360" height="100"><defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="160" height="70"><feImage id="sourceImage" width="160" height="70" href=")" +
         ChildUri() +
         R"svg("/></filter></defs><rect id="first" width="160" height="70" filter="url(#f)"/><rect id="unrelated" x="345" y="80" width="10" height="10"/></svg>)svg";
}

SVGDocumentHandle ChildHandle(SVGDocument& document, std::string_view selector) {
  const auto element = document.querySelector(selector);
  if (!element) return {};
  const auto* image = document.registry().try_get<components::LoadedSVGImageComponent>(
      element->unsafeEntityHandle().entity());
  return image ? image->subDocument : SVGDocumentHandle{};
}

bool StageInter(CatalogEncodedFontStore& store, const FontCatalog& catalog) {
  const auto id = catalog.availability("Inter", {}).contentId;
  if (!store.queue(id)) return false;
  const auto token = store.beginFetch(id);
  return token &&
         store.publishVerified(id, token, EmbeddedFontProvider().loadFamilyData("Inter", {}));
}

void ExpectChildRefresh(std::string_view source, std::string_view selector, std::string name) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  FontCatalog catalog(store);
  ScopedDefaultProvider defaults(catalog);
  auto parsed = ParseDocument(source);
  ASSERT_EQ(parsed.has_value(), true);
  auto& document = *parsed;
  const auto host = *document.querySelector("#first");
  EXPECT_EQ(document.preflightFontResourcesForElement(host).status,
            FontResourcePreflight::Status::NeedsRender);
  Renderer renderer;
  renderer.draw(document);
  const auto child = ChildHandle(document, selector);
  ASSERT_THAT(child, Not(testing::IsNull()));
  EXPECT_EQ(document.hasUnresolvedFontResources(), true);
  EXPECT_THAT(document.renderedFontResources().dependencies,
              Each(Field(&FontFaceDependency::state, FontFaceLoadState::WaitingForBytes)));
  ASSERT_THAT(document.fontDependenciesForElement(host), SizeIs(1));
  EXPECT_EQ(document.fontDependenciesForElement(host)[0].family, "inter");
  auto& childManager = child->registry().ctx().get<FontManager>();
  EXPECT_EQ(childManager.compressedFontDecompressionAttempts(), 0u);
  EXPECT_THAT(document.registry().ctx().get<FontManager>().faceDependencies(), IsEmpty());
  const auto sourceBefore = std::string(document.source());
  EXPECT_EQ(sourceBefore, source);
  const Entity unrelated = document.querySelector("#unrelated")->unsafeEntityHandle().entity();
  ASSERT_EQ(StageInter(*store, catalog), true);
  EXPECT_EQ(document.refreshFontResources(), false);
  EXPECT_EQ(childManager.compressedFontDecompressionAttempts(), 0u);
  ASSERT_EQ(store->adoptReadyAssets(), true);
  ASSERT_EQ(document.refreshFontResources(), true);
  EXPECT_EQ(childManager.compressedFontDecompressionAttempts(), 1u);
  EXPECT_EQ(document.registry().all_of<components::DirtyFlagsComponent>(
                host.unsafeEntityHandle().entity()),
            true);
  EXPECT_EQ(document.registry().all_of<components::DirtyFlagsComponent>(unrelated), false);
  EXPECT_EQ(document.refreshFontResources(), false);
  renderer.draw(document);
  const auto actual = renderer.takeSnapshot();
  EXPECT_EQ(document.hasUnresolvedFontResources(), false);
  EXPECT_EQ(document.preflightFontResourcesForElement(host).status,
            FontResourcePreflight::Status::Ready);
  EXPECT_EQ(document.source(), sourceBefore);

  FontCatalog embedded;
  ScopedDefaultProvider nativeDefaults(embedded);
  auto native = ParseDocument(source);
  ASSERT_EQ(native.has_value(), true);
  Renderer nativeRenderer;
  nativeRenderer.draw(*native);
  ::donner::editor::tests::CompareBitmapToBitmap(
      actual, nativeRenderer.takeSnapshot(), name,
      ::donner::editor::tests::PixelmatchIdentityParams());
}

TEST(NestedSvgFontResourcesTest, ImageOnlyParentLoadsAndRefreshesActualChildFont) {
  ExpectChildRefresh(ImageSource(), "#first", "nested_image_font_adoption");
}

TEST(NestedSvgFontResourcesTest, SvgFeImagePublishesAndRefreshesActualChildFont) {
  ExpectChildRefresh(FilterSource(), "#sourceImage", "nested_feimage_font_adoption");
}

TEST(NestedSvgFontResourcesTest, UnpreparedChildIsNotReadyAfterOtherPaintPreparation) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  FontCatalog catalog(store);
  ScopedDefaultProvider defaults(catalog);
  auto parsed = ParseDocument(ImageSource());
  ASSERT_EQ(parsed.has_value(), true);
  auto& document = *parsed;
  const auto host = *document.querySelector("#first");
  EXPECT_EQ(document.preflightFontResourcesForElement(host).status,
            FontResourcePreflight::Status::NeedsRender);
  // A filter preparation can complete before the image's separate registry has been traversed.
  document.registry()
      .get_or_emplace<components::FontPaintDependenciesComponent>(
          host.unsafeEntityHandle().entity())
      .prepared = true;
  EXPECT_EQ(document.preflightFontResourcesForElement(host).status,
            FontResourcePreflight::Status::NeedsRender);
  EXPECT_EQ(store->availability(catalog.availability("Inter", {}).contentId).state,
            FontAssetState::Absent);
}

TEST(NestedSvgFontResourcesTest, RecursiveChildHandleFailsWithinTheExistingResourceBound) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  FontCatalog catalog(store);
  ScopedDefaultProvider defaults(catalog);
  auto parsed = ParseDocument(ImageSource());
  ASSERT_EQ(parsed.has_value(), true);
  auto& document = *parsed;
  const auto host = *document.querySelector("#first");
  (void)document.preflightFontResourcesForElement(host);
  auto& image = document.registry().get<components::LoadedSVGImageComponent>(
      host.unsafeEntityHandle().entity());
  const auto original = image.subDocument;
  image.subDocument = document.handle();
  Renderer renderer;
  renderer.draw(document);
  EXPECT_EQ(document.fontResourcesExceeded(), true);
  EXPECT_EQ(document.preflightFontResourcesForElement(host).status,
            FontResourcePreflight::Status::ResourceLimit);
  // Remove the deliberately constructed strong cycle before the fixture's document dies.
  image.subDocument = original;
}

TEST(NestedSvgFontResourcesTest, SharedCachedChildRefreshesOnceAndDirtiesBothHosts) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  FontCatalog catalog(store);
  ScopedDefaultProvider defaults(catalog);
  auto parsed = ParseDocument(ImageSource(true));
  ASSERT_EQ(parsed.has_value(), true);
  auto& document = *parsed;
  Renderer renderer;
  renderer.draw(document);
  const auto first = ChildHandle(document, "#first");
  const auto second = ChildHandle(document, "#second");
  ASSERT_THAT(first, Not(testing::IsNull()));
  EXPECT_EQ(first, second);
  ASSERT_EQ(StageInter(*store, catalog), true);
  ASSERT_EQ(store->adoptReadyAssets(), true);
  EXPECT_EQ(document.refreshFontResources(), true);
  EXPECT_EQ(first->registry().ctx().get<FontManager>().compressedFontDecompressionAttempts(), 1u);
  const auto& work = document.registry().ctx().get<components::FontResourceGraphCache::Stats>();
  EXPECT_LE(work.graphBuilds, 2u);        // The parent and the one shared child.
  EXPECT_LE(work.targetCollections, 2u);  // Complete-target and rendered-frame purposes.
  for (const auto selector : {"#first", "#second"}) {
    const auto host = document.querySelector(selector)->unsafeEntityHandle().entity();
    EXPECT_EQ(document.registry().all_of<components::DirtyFlagsComponent>(host), true);
    const auto& paint = document.registry().get<components::FontPaintDependenciesComponent>(host);
    ASSERT_THAT(paint.children, SizeIs(1));
    EXPECT_EQ(paint.children[0].document.lock(), first);
    ASSERT_THAT(paint.children[0].fontDependencies, SizeIs(1));
    EXPECT_EQ(paint.children[0].fontDependencies[0].family, "inter");
    EXPECT_THAT(paint.children[0].fontDependencies,
                Each(Field(&FontFaceDependency::state, FontFaceLoadState::Loaded)));
  }
  EXPECT_EQ(document.refreshFontResources(), false);
}

TEST(NestedSvgFontResourcesTest, ParentLoadedFaceCannotHideChildWaitingForSharedSlot) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  FontCatalog catalog(store);
  ScopedDefaultProvider defaults(catalog);
  ASSERT_EQ(StageInter(*store, catalog), true);
  ASSERT_EQ(store->adoptReadyAssets(), true);
  auto parsed = ParseDocument(ImageSource(false, true));
  ASSERT_EQ(parsed.has_value(), true);
  auto& document = *parsed;
  auto& registry = document.registry();
  auto& parentManager = registry.ctx().emplace<FontManager>(registry);
  EXPECT_NE(parentManager.findFont("Inter"), parentManager.fallbackFont());
  const auto id = catalog.availability("Inter", {}).contentId;
  auto occupied = store->tryAcquireDecode(id);
  Renderer renderer;
  renderer.draw(document);
  const auto target = *document.querySelector("#first");
  const auto parentText = *document.querySelector("#parentText");
  EXPECT_THAT(document.fontDependenciesForElement(parentText),
              Each(Field(&FontFaceDependency::state, FontFaceLoadState::Loaded)));
  ASSERT_THAT(document.fontDependenciesForElement(target), SizeIs(1));
  EXPECT_EQ(document.fontDependenciesForElement(target)[0].waitReason,
            FontFaceWaitReason::SharedDecodeSlot);
  ASSERT_THAT(document.renderedFontResources().dependencies, SizeIs(1));
  EXPECT_EQ(document.renderedFontResources().dependencies[0].state,
            FontFaceLoadState::WaitingForAdmission);
  const auto generation = store->availability(id).contentGeneration;
  occupied.reservation.reset();
  EXPECT_EQ(document.refreshFontResources(), true);
  EXPECT_EQ(store->availability(id).contentGeneration, generation);
  EXPECT_EQ(store->beginFetch(id), 0u);
  renderer.draw(document);
  EXPECT_EQ(document.hasUnresolvedFontResources(), false);
}

TEST(NestedSvgFontResourcesTest, UncachedBackendLayoutPublishesPendingFontDependencies) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  FontCatalog catalog(store);
  ScopedDefaultProvider defaults(catalog);
  auto parsed = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="160" height="70"><text id="label" x="3" y="43" font-family="Inter" font-size="35">WiWmmm</text></svg>)");
  ASSERT_TRUE(parsed.has_value());
  testing::NiceMock<tests::MockRendererInterface> backend;
  EXPECT_CALL(backend, drawText(testing::_, testing::_, testing::_))
      .WillOnce([](Registry& registry, const components::ComputedTextComponent& text,
                   const TextParams& params) {
        TextLayoutParams layout;
        layout.fontFamilies = params.fontFamilies;
        layout.fontSize = params.fontSize;
        layout.viewBox = params.viewBox;
        layout.fontMetrics = params.fontMetrics;
        EXPECT_THAT(registry.ctx().get<TextEngine>().layout(text, layout), Not(IsEmpty()));
      });
  RendererDriver driver(backend);
  driver.draw(*parsed);
  const auto pending = parsed->registry().ctx().get<FontManager>().faceDependencies();
  ASSERT_THAT(pending, SizeIs(1));
  EXPECT_EQ(pending[0].family, "inter");
  EXPECT_EQ(pending[0].state, FontFaceLoadState::WaitingForBytes);
  const auto fonts = parsed->renderedFontResources();
  EXPECT_EQ(fonts.status, FontResourcePreflight::Status::PendingFonts);
  EXPECT_THAT(fonts.dependencies, testing::ElementsAre(pending[0]));
  const auto label = *parsed->querySelector("#label");
  EXPECT_THAT(parsed->fontDependenciesForElement(label), testing::ElementsAre(pending[0]));
}

TEST(NestedSvgFontResourcesTest, MissingOffscreenRendererPreservesUnpreparedChildStatus) {
  auto parsed = ParseDocument(FilterSource());
  ASSERT_TRUE(parsed.has_value());
  testing::NiceMock<tests::MockRendererInterface> backend;
  EXPECT_CALL(backend, createOffscreenInstance()).WillOnce(testing::Return(nullptr));
  RendererDriver driver(backend);
  driver.draw(*parsed);
  const auto fonts = parsed->renderedFontResources();
  EXPECT_EQ(fonts.status, FontResourcePreflight::Status::NeedsRender);
  EXPECT_THAT(fonts.dependencies, IsEmpty());
}

TEST(NestedSvgFontResourcesTest, CulledChildExclusionAppliesOnlyToCompletedRenderedScope) {
  std::string source = ImageSource();
  source.replace(source.find("id='first'"), std::string("id='first'").size(),
                 "id='first' x='1000'");
  auto parsed = ParseDocument(source);
  ASSERT_TRUE(parsed.has_value());
  testing::NiceMock<tests::MockRendererInterface> backend;
  RendererDriver driver(backend);
  driver.draw(*parsed);
  EXPECT_EQ(parsed->renderedFontResources().status, FontResourcePreflight::Status::Ready);
  const auto host = *parsed->querySelector("#first");
  EXPECT_EQ(parsed->preflightFontResourcesForElement(host).status,
            FontResourcePreflight::Status::NeedsRender);
  // A cancelled later draw cannot publish or inherit the previous viewport's exclusions.
  EXPECT_FALSE(driver.drawInterruptibly(*parsed, RenderViewport{.size = Vector2d(360, 100)},
                                        Transform2d(), [] { return true; }));
  EXPECT_EQ(parsed->renderedFontResources().status, FontResourcePreflight::Status::NeedsRender);
}

TEST(NestedSvgFontResourcesTest, RepeatedChildTargetsReuseCollectionWithinOnePreparation) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  FontCatalog catalog(store);
  ScopedDefaultProvider defaults(catalog);
  constexpr int kHosts = 24;
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="360" height="100">)";
  for (int i = 0; i < kHosts; ++i) {
    source += "<image width='160' height='70' href='" + ChildUri() + "'/>";
  }
  source += "</svg>";
  auto parsed = ParseDocument(source);
  ASSERT_TRUE(parsed.has_value());
  testing::NiceMock<tests::MockRendererInterface> backend;
  ON_CALL(backend, drawText(testing::_, testing::_, testing::_))
      .WillByDefault(
          [](Registry& registry, const components::ComputedTextComponent& text, const TextParams&) {
            ASSERT_FALSE(text.spans.empty());
            (void)registry.ctx().get<TextEngine>().ensureComputedTextGeometryComponent(
                EntityHandle(registry, text.spans.front().sourceEntity));
          });
  RendererDriver::SecurityStats stats;
  RendererDriver driver(backend, false, &stats);
  driver.draw(*parsed);
  const auto fonts = parsed->renderedFontResources();
  EXPECT_EQ(fonts.status, FontResourcePreflight::Status::PendingFonts);
  ASSERT_THAT(fonts.dependencies, SizeIs(1));
  EXPECT_EQ(fonts.dependencies[0].family, "inter");
  EXPECT_LE(stats.nestedFontResources.graphBuilds, 1u);
  EXPECT_LE(stats.nestedFontResources.targetCollections, 2u);
  EXPECT_FALSE(stats.nestedFontResources.resourceLimit);
  // A new draw starts a new operation even when the parsed child and font revision are unchanged.
  driver.draw(*parsed);
  ASSERT_THAT(parsed->renderedFontResources().dependencies, SizeIs(1));
  EXPECT_LE(stats.nestedFontResources.graphBuilds, 1u);
  EXPECT_LE(stats.nestedFontResources.targetCollections, 2u);
}

TEST(NestedSvgFontResourcesTest, DistinctTargetCollectionWorkStopsAtTheAggregateCeiling) {
  constexpr int kTargets = 512;
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)";
  for (int i = 0; i < kTargets; ++i) {
    source += "<rect id='r" + std::to_string(i) + "' width='1' height='1'/>";
  }
  source += "</svg>";
  auto parsed = ParseDocument(source);
  ASSERT_TRUE(parsed.has_value());
  testing::NiceMock<tests::MockRendererInterface> backend;
  RendererDriver driver(backend);
  driver.draw(*parsed);
  components::FontResourceGraphCache::Stats work;
  components::FontResourceGraphCache cache(&work);
  bool rejected = false;
  for (int i = 0; i < kTargets; ++i) {
    const auto target = parsed->querySelector("#r" + std::to_string(i));
    ASSERT_TRUE(target.has_value());
    rejected |=
        cache.collect(parsed->handle(), target->unsafeEntityHandle().entity()).resourceLimit;
  }
  EXPECT_TRUE(rejected);
  EXPECT_TRUE(work.resourceLimit);
  EXPECT_LE(work.graphBuilds, 1u);
  EXPECT_GT(work.targetCollections, 1u);
  EXPECT_LT(work.targetCollections, kTargets);
  EXPECT_LE(work.work, components::FontResourceGraphCache::kMaximumWork);
}

TEST(NestedSvgFontResourcesTest, TwoStandaloneRangesCannotQualifyAWholeDocument) {
  std::string source = ImageSource();
  source.replace(source.find("id='first'"), std::string("id='first'").size(),
                 "id='first' x='1000'");
  auto parsed = ParseDocument(source);
  ASSERT_TRUE(parsed.has_value());
  testing::NiceMock<tests::MockRendererInterface> backend;
  RendererDriver driver(backend);
  driver.draw(*parsed);
  EXPECT_EQ(parsed->renderedFontResources().status, FontResourcePreflight::Status::Ready);
  const Entity image = parsed->querySelector("#first")->unsafeEntityHandle().entity();
  const Entity unrelated = parsed->querySelector("#unrelated")->unsafeEntityHandle().entity();
  auto& registry = parsed->registry();
  ASSERT_TRUE(registry.ctx().get<components::RenderedFontResourceScope>().excluded.contains(image));
  const RenderViewport viewport{.size = Vector2d(360, 100)};
  driver.drawEntityRange(registry, unrelated, unrelated, viewport, Transform2d());
  EXPECT_EQ(parsed->renderedFontResources().status, FontResourcePreflight::Status::NeedsRender);
  driver.drawEntityRangeIntoCurrentFrame(registry, unrelated, unrelated, viewport, Transform2d());
  EXPECT_EQ(parsed->renderedFontResources().status, FontResourcePreflight::Status::NeedsRender);
  EXPECT_EQ(registry.ctx().get<components::RenderedFontResourceScope>().coverageRoot,
            Entity(entt::null));
}

TEST(NestedSvgFontResourcesTest, FragmentCoverageDoesNotQualifyAnotherTargetOrWholeDocument) {
  auto parsed = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect id="a" width="2" height="2"/><rect id="b" x="10" width="2" height="2"/></svg>)");
  ASSERT_TRUE(parsed.has_value());
  testing::NiceMock<tests::MockRendererInterface> backend;
  RendererDriver driver(backend);
  driver.draw(*parsed);
  auto& registry = parsed->registry();
  const Entity first = parsed->querySelector("#a")->unsafeEntityHandle().entity();
  const Entity second = parsed->querySelector("#b")->unsafeEntityHandle().entity();
  components::FontResourceGraphCache cache;
  components::ScopedFontResourceRender firstScope(registry, first);
  firstScope.finish();
  EXPECT_FALSE(
      cache.collect(parsed->handle(), first, components::FontResourceGraph::Purpose::RenderedFrame)
          .needsRender);
  EXPECT_EQ(parsed->renderedFontResources().status, FontResourcePreflight::Status::NeedsRender);
  components::ScopedFontResourceRender secondScope(registry, second);
  secondScope.finish();
  EXPECT_TRUE(
      cache.collect(parsed->handle(), first, components::FontResourceGraph::Purpose::RenderedFrame)
          .needsRender);
  EXPECT_FALSE(
      cache.collect(parsed->handle(), second, components::FontResourceGraph::Purpose::RenderedFrame)
          .needsRender);
}

TEST(NestedSvgFontResourcesTest, NestedCancellationPoisonsEnclosingFullCoverage) {
  auto parsed = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect width="2" height="2"/></svg>)");
  ASSERT_TRUE(parsed.has_value());
  testing::NiceMock<tests::MockRendererInterface> backend;
  RendererDriver driver(backend);
  driver.draw(*parsed);
  auto& registry = parsed->registry();
  const Entity root = registry.ctx().get<components::SVGDocumentContext>().rootEntity;
  components::ScopedFontResourceRender outer(registry, root);
  components::ScopedFontResourceRender inner(registry);
  inner.finish(false);
  outer.finish(true);
  EXPECT_FALSE(registry.ctx().get<components::RenderedFontResourceScope>().complete);
  EXPECT_EQ(parsed->renderedFontResources().status, FontResourcePreflight::Status::NeedsRender);
}

TEST(NestedSvgFontResourcesTest, IncompleteRenderedMissDoesNotPoisonLaterCompletedScope) {
  auto parsed = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect width="2" height="2"/></svg>)");
  ASSERT_TRUE(parsed.has_value());
  testing::NiceMock<tests::MockRendererInterface> backend;
  RendererDriver driver(backend);
  driver.draw(*parsed);
  auto& registry = parsed->registry();
  const Entity root = registry.ctx().get<components::SVGDocumentContext>().rootEntity;
  components::FontResourceGraphCache::Stats stats;
  components::FontResourceGraphCache cache(&stats);
  components::ScopedFontResourceRender scope(registry, root);
  EXPECT_TRUE(
      cache.collect(parsed->handle(), root, components::FontResourceGraph::Purpose::RenderedFrame)
          .needsRender);
  EXPECT_EQ(stats.graphBuilds, 0u);
  EXPECT_EQ(stats.targetCollections, 0u);
  // CompleteTarget evidence is permitted during preparation, but cannot freeze incomplete render
  // proof.
  EXPECT_FALSE(cache.collect(parsed->handle(), root).needsRender);
  scope.finish();
  EXPECT_FALSE(
      cache.collect(parsed->handle(), root, components::FontResourceGraph::Purpose::RenderedFrame)
          .needsRender);
}

TEST(NestedSvgFontResourcesTest, FragmentRangeProofRejectsUnrenderedGroupDescendants) {
  auto child = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="group"><rect id="leaf" width="2" height="2"/></g><g id="layer" opacity="0.5"><rect width="2" height="2"/></g></svg>)");
  ASSERT_TRUE(child.has_value());
  for (const auto& [fragment, expected] :
       {std::pair{"group", FontResourcePreflight::Status::NeedsRender},
        std::pair{"leaf", FontResourcePreflight::Status::Ready},
        std::pair{"layer", FontResourcePreflight::Status::Ready}}) {
    SCOPED_TRACE(fragment);
    auto parent = ParseDocument(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="host"/></svg>)");
    ASSERT_TRUE(parent.has_value());
    const auto host = parent->querySelector("#host")->unsafeEntityHandle().entity();
    // Install an already-loaded external reference to exercise the driver without network I/O.
    parent->registry().emplace<components::ExternalUseComponent>(host, child->handle(),
                                                                 RcString(fragment));
    testing::NiceMock<tests::MockRendererInterface> backend;
    RendererDriver driver(backend);
    driver.draw(*parent);
    EXPECT_EQ(parent->renderedFontResources().status, expected);
    const auto& captured = parent->registry().get<components::FontPaintDependenciesComponent>(host);
    ASSERT_THAT(captured.children, SizeIs(1));
    EXPECT_EQ(captured.children[0].renderedNeedsRender,
              expected == FontResourcePreflight::Status::NeedsRender);
  }
}

TEST(NestedSvgFontResourcesTest, PaintSnapshotsDoNotKeepDiscardedPreviewChildrenAlive) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  FontCatalog catalog(store);
  ScopedDefaultProvider defaults(catalog);
  std::vector<components::ChildFontPaintDependencies> retainedSnapshots;
  {
    auto temporary = ParseDocument(ImageSource());
    ASSERT_EQ(temporary.has_value(), true);
    auto& document = *temporary;
    Renderer renderer;
    renderer.draw(document);
    const auto host = document.querySelector("#first")->unsafeEntityHandle().entity();
    retainedSnapshots =
        document.registry().get<components::FontPaintDependenciesComponent>(host).children;
    ASSERT_THAT(retainedSnapshots, SizeIs(1));
    EXPECT_EQ(retainedSnapshots[0].document.expired(), false);
  }
  EXPECT_EQ(retainedSnapshots[0].document.expired(), true);
  EXPECT_THAT(retainedSnapshots[0].fontDependencies, SizeIs(1));
  ASSERT_EQ(StageInter(*store, catalog), true);
  EXPECT_EQ(store->adoptReadyAssets(), true);
  EXPECT_EQ(retainedSnapshots[0].document.expired(), true);
}

}  // namespace
}  // namespace donner::svg
