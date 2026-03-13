#include "donner/svg/renderer/CompositedRenderer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/components/LayerMembershipComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

using ::testing::Eq;

/// Compare two bitmaps pixel-by-pixel, returning the number of differing pixels.
int countDifferingPixels(const RendererBitmap& a, const RendererBitmap& b) {
  EXPECT_EQ(a.dimensions, b.dimensions);
  if (a.dimensions != b.dimensions) {
    return -1;
  }

  int diffCount = 0;
  const int width = a.dimensions.x;
  const int height = a.dimensions.y;
  const size_t stride = static_cast<size_t>(width) * 4;
  const size_t aRowBytes = a.rowBytes > 0 ? a.rowBytes : stride;
  const size_t bRowBytes = b.rowBytes > 0 ? b.rowBytes : stride;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t aOff = static_cast<size_t>(y) * aRowBytes + static_cast<size_t>(x) * 4;
      const size_t bOff = static_cast<size_t>(y) * bRowBytes + static_cast<size_t>(x) * 4;
      if (a.pixels[aOff] != b.pixels[bOff] || a.pixels[aOff + 1] != b.pixels[bOff + 1] ||
          a.pixels[aOff + 2] != b.pixels[bOff + 2] || a.pixels[aOff + 3] != b.pixels[bOff + 3]) {
        ++diffCount;
      }
    }
  }

  return diffCount;
}

/// Find the render entity corresponding to a data entity with the given ID.
Entity findRenderEntityById(Registry& registry, const char* id) {
  auto& docCtx = registry.ctx().get<const components::SVGDocumentContext>();
  const Entity dataEntity = docCtx.getEntityById(RcString(id));
  if (dataEntity == entt::null) {
    return Entity(entt::null);
  }

  auto view = registry.view<components::RenderingInstanceComponent>();
  for (auto entity : view) {
    const auto& instance = view.get<components::RenderingInstanceComponent>(entity);
    if (instance.dataEntity == dataEntity) {
      return entity;
    }
  }
  return Entity(entt::null);
}

class CompositedRendererTest : public ::testing::Test {
protected:
  SVGDocument makeDocument(std::string_view svg, Vector2i size = kTestSvgDefaultSize) {
    return instantiateSubtree(svg, parser::SVGParser::Options(), size);
  }

  RendererBitmap renderSinglePass(SVGDocument& document) {
    return RenderDocumentWithActiveBackend(document);
  }
};

TEST_F(CompositedRendererTest, NoPromotedEntitiesMatchesSinglePass) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="2" y="2" width="12" height="12" fill="red" />
  )svg");
  RendererBitmap reference = renderSinglePass(document);
  ASSERT_FALSE(reference.empty());

  SVGDocument document2 = makeDocument(R"svg(
    <rect x="2" y="2" width="12" height="12" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);
  compositor.prepare(document2, {});
  compositor.renderFrame();
  RendererBitmap composited = compositor.takeSnapshot();

  ASSERT_FALSE(composited.empty());
  EXPECT_THAT(composited.dimensions, Eq(reference.dimensions));
  EXPECT_EQ(countDifferingPixels(reference, composited), 0)
      << "Composited output should be pixel-identical to single-pass with no promoted entities";
}

TEST_F(CompositedRendererTest, MultipleRectsNoPromotionMatchesSinglePass) {
  const std::string_view svg = R"svg(
    <rect x="1" y="1" width="6" height="6" fill="blue" />
    <rect x="4" y="4" width="8" height="8" fill="green" />
    <rect x="8" y="2" width="6" height="12" fill="red" opacity="0.5" />
  )svg";

  SVGDocument document1 = makeDocument(svg);
  RendererBitmap reference = renderSinglePass(document1);

  SVGDocument document2 = makeDocument(svg);
  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);
  compositor.prepare(document2, {});
  compositor.renderFrame();
  RendererBitmap composited = compositor.takeSnapshot();

  ASSERT_FALSE(composited.empty());
  EXPECT_EQ(countDifferingPixels(reference, composited), 0);
}

TEST_F(CompositedRendererTest, LayerCountNoPromotion) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="1" y="1" width="6" height="6" fill="blue" />
    <rect x="4" y="4" width="8" height="8" fill="green" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);
  compositor.prepare(document, {});

  EXPECT_EQ(compositor.layers().layers.size(), 1u);
  EXPECT_EQ(compositor.layers().layers[0].reason, CompositingLayer::Reason::Static);
}

TEST_F(CompositedRendererTest, PromotedEntityCreatesMultipleLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="bg" x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
    <rect id="fg" x="6" y="6" width="4" height="4" fill="blue" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");
  ASSERT_NE(animatedEntity, Entity(entt::null));

  compositor.prepare(document, {animatedEntity});

  EXPECT_GE(compositor.layers().layers.size(), 2u);

  bool hasDynamic = false;
  for (const auto& layer : compositor.layers().layers) {
    if (layer.reason == CompositingLayer::Reason::Animation) {
      hasDynamic = true;
      break;
    }
  }
  EXPECT_TRUE(hasDynamic);
}

TEST_F(CompositedRendererTest, PromotedEntityCompositeMatchesSinglePass) {
  const std::string_view svg = R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
    <rect x="6" y="6" width="4" height="4" fill="blue" />
  )svg";

  SVGDocument document1 = makeDocument(svg);
  RendererBitmap reference = renderSinglePass(document1);

  SVGDocument document2 = makeDocument(svg);
  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document2, false);
  Entity animatedEntity = findRenderEntityById(document2.registry(), "animated");
  ASSERT_NE(animatedEntity, Entity(entt::null));

  compositor.prepare(document2, {animatedEntity});
  compositor.renderFrame();
  RendererBitmap composited = compositor.takeSnapshot();

  ASSERT_FALSE(composited.empty());
  EXPECT_THAT(composited.dimensions, Eq(reference.dimensions));
  EXPECT_EQ(countDifferingPixels(reference, composited), 0)
      << "Composited output with promoted entity should match single-pass";
}

TEST_F(CompositedRendererTest, DirtyTrackingOnlyDirtyLayersRerender) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");
  ASSERT_NE(animatedEntity, Entity(entt::null));

  compositor.prepare(document, {animatedEntity});

  // First render — all layers dirty.
  compositor.renderFrame();
  RendererBitmap first = compositor.takeSnapshot();
  ASSERT_FALSE(first.empty());

  // Second render — no layers dirty, output should be identical.
  compositor.renderFrame();
  RendererBitmap second = compositor.takeSnapshot();
  ASSERT_FALSE(second.empty());
  EXPECT_EQ(countDifferingPixels(first, second), 0);

  // Mark one layer dirty and render again — still same visual output.
  if (!compositor.layers().layers.empty()) {
    compositor.markLayerDirty(0);
  }
  compositor.renderFrame();
  RendererBitmap third = compositor.takeSnapshot();
  EXPECT_EQ(countDifferingPixels(first, third), 0);
}

TEST_F(CompositedRendererTest, MarkAllDirty) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="2" y="2" width="12" height="12" fill="green" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);
  compositor.prepare(document, {});
  compositor.renderFrame();
  RendererBitmap before = compositor.takeSnapshot();

  compositor.markAllDirty();
  compositor.renderFrame();
  RendererBitmap after = compositor.takeSnapshot();

  EXPECT_EQ(countDifferingPixels(before, after), 0)
      << "Re-rendering all layers should produce identical output";
}

TEST_F(CompositedRendererTest, OpacityGroupCompositeMatchesSinglePass) {
  const std::string_view svg = R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <g opacity="0.5">
      <rect x="2" y="2" width="6" height="6" fill="red" />
      <rect x="4" y="4" width="6" height="6" fill="blue" />
    </g>
  )svg";

  SVGDocument document1 = makeDocument(svg);
  RendererBitmap reference = renderSinglePass(document1);

  SVGDocument document2 = makeDocument(svg);
  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);
  compositor.prepare(document2, {});
  compositor.renderFrame();
  RendererBitmap composited = compositor.takeSnapshot();

  ASSERT_FALSE(composited.empty());
  EXPECT_EQ(countDifferingPixels(reference, composited), 0);
}

// --- Phase 3: Frame stats, entity dirty marking, animation invalidation ---

TEST_F(CompositedRendererTest, FrameStatsFirstRenderAllDirty) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");
  ASSERT_NE(animatedEntity, Entity(entt::null));

  compositor.prepare(document, {animatedEntity});
  compositor.renderFrame();

  const auto& stats = compositor.lastFrameStats();
  EXPECT_EQ(stats.layersRasterized, compositor.layers().layers.size());
  EXPECT_EQ(stats.layersReused, 0u);
}

TEST_F(CompositedRendererTest, FrameStatsSecondRenderNoDirty) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");
  ASSERT_NE(animatedEntity, Entity(entt::null));

  compositor.prepare(document, {animatedEntity});
  compositor.renderFrame();

  // Second render — nothing dirty.
  compositor.renderFrame();
  const auto& stats = compositor.lastFrameStats();
  EXPECT_EQ(stats.layersRasterized, 0u);
  EXPECT_EQ(stats.layersReused, compositor.layers().layers.size());
}

TEST_F(CompositedRendererTest, FrameStatsAfterMarkLayerDirty) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
    <rect x="6" y="6" width="4" height="4" fill="blue" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");
  ASSERT_NE(animatedEntity, Entity(entt::null));

  compositor.prepare(document, {animatedEntity});
  compositor.renderFrame();

  // Mark just one layer dirty.
  compositor.markLayerDirty(0);
  compositor.renderFrame();

  const auto& stats = compositor.lastFrameStats();
  EXPECT_EQ(stats.layersRasterized, 1u);
  EXPECT_EQ(stats.layersReused, compositor.layers().layers.size() - 1);
}

TEST_F(CompositedRendererTest, MarkEntityDirtyViaDataEntity) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="target" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity targetRenderEntity = findRenderEntityById(document.registry(), "target");
  ASSERT_NE(targetRenderEntity, Entity(entt::null));

  compositor.prepare(document, {targetRenderEntity});
  compositor.renderFrame();

  // Mark dirty using the data entity (not the render entity).
  auto& docCtx = document.registry().ctx().get<const components::SVGDocumentContext>();
  Entity dataEntity = docCtx.getEntityById(RcString("target"));
  ASSERT_NE(dataEntity, Entity(entt::null));

  bool found = compositor.markEntityDirty(dataEntity);
  EXPECT_TRUE(found);

  compositor.renderFrame();
  EXPECT_GE(compositor.lastFrameStats().layersRasterized, 1u);
}

TEST_F(CompositedRendererTest, MarkEntityDirtyViaRenderEntity) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="target" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity targetRenderEntity = findRenderEntityById(document.registry(), "target");
  ASSERT_NE(targetRenderEntity, Entity(entt::null));

  compositor.prepare(document, {targetRenderEntity});
  compositor.renderFrame();

  // Mark dirty using the render entity directly.
  bool found = compositor.markEntityDirty(targetRenderEntity);
  EXPECT_TRUE(found);

  compositor.renderFrame();
  EXPECT_GE(compositor.lastFrameStats().layersRasterized, 1u);
}

TEST_F(CompositedRendererTest, MarkEntityDirtyUnknownEntity) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="2" y="2" width="12" height="12" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);
  compositor.prepare(document, {});
  compositor.renderFrame();

  // An entity that doesn't exist should return false.
  Entity bogus = Entity(entt::null);
  EXPECT_FALSE(compositor.markEntityDirty(bogus));
}

TEST_F(CompositedRendererTest, InvalidateAnimatedLayersWithNoAnimations) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="2" y="2" width="12" height="12" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);
  compositor.prepare(document, {});
  compositor.renderFrame();

  uint32_t dirtyCount = compositor.invalidateAnimatedLayers();
  EXPECT_EQ(dirtyCount, 0u);

  // Second render should reuse all layers.
  compositor.renderFrame();
  EXPECT_EQ(compositor.lastFrameStats().layersRasterized, 0u);
}

TEST_F(CompositedRendererTest, InvalidateAnimatedLayersWithManualAnimation) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="target" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity targetRenderEntity = findRenderEntityById(document.registry(), "target");
  ASSERT_NE(targetRenderEntity, Entity(entt::null));

  compositor.prepare(document, {targetRenderEntity});
  compositor.renderFrame();

  // Simulate animation: manually attach AnimatedValuesComponent to the data entity.
  auto& docCtx = document.registry().ctx().get<const components::SVGDocumentContext>();
  Entity dataEntity = docCtx.getEntityById(RcString("target"));
  ASSERT_NE(dataEntity, Entity(entt::null));

  auto& animValues =
      document.registry().emplace<components::AnimatedValuesComponent>(dataEntity);
  animValues.overrides["fill"] = "blue";

  uint32_t dirtyCount = compositor.invalidateAnimatedLayers();
  EXPECT_GE(dirtyCount, 1u);

  compositor.renderFrame();
  EXPECT_GE(compositor.lastFrameStats().layersRasterized, 1u);
}

// --- Phase 4: Editor integration, transform-only composition ---

TEST_F(CompositedRendererTest, SelectionLayerPolicy) {
  SelectionLayerPolicy policy;
  EXPECT_TRUE(policy.selectedEntities().empty());

  Entity e1 = Entity(entt::null);  // Placeholder; policy doesn't validate entities.
  policy.addEntity(e1);
  EXPECT_EQ(policy.selectedEntities().size(), 1u);

  policy.clear();
  EXPECT_TRUE(policy.selectedEntities().empty());

  std::vector<Entity> entities = {Entity(entt::null), Entity(entt::null)};
  policy.setSelectedEntities(entities);
  EXPECT_EQ(policy.selectedEntities().size(), 2u);
}

TEST_F(CompositedRendererTest, FindLayerForEntity) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="target" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity targetRenderEntity = findRenderEntityById(document.registry(), "target");
  ASSERT_NE(targetRenderEntity, Entity(entt::null));

  compositor.prepare(document, {targetRenderEntity});

  // Should find the layer for both render entity and data entity.
  auto layerViaRender = compositor.findLayerForEntity(targetRenderEntity);
  EXPECT_TRUE(layerViaRender.has_value());

  auto& docCtx = document.registry().ctx().get<const components::SVGDocumentContext>();
  Entity dataEntity = docCtx.getEntityById(RcString("target"));
  auto layerViaData = compositor.findLayerForEntity(dataEntity);
  EXPECT_TRUE(layerViaData.has_value());

  // Both should resolve to the same layer.
  EXPECT_EQ(*layerViaRender, *layerViaData);

  // Unknown entity returns nullopt.
  EXPECT_FALSE(compositor.findLayerForEntity(Entity(entt::null)).has_value());
}

TEST_F(CompositedRendererTest, TransformOnlyCompositionTranslation) {
  const std::string_view svg = R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="dragged" x="4" y="4" width="4" height="4" fill="red" />
  )svg";

  SVGDocument document = makeDocument(svg);
  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity draggedEntity = findRenderEntityById(document.registry(), "dragged");
  ASSERT_NE(draggedEntity, Entity(entt::null));

  compositor.prepare(document, {draggedEntity});
  compositor.renderFrame();
  RendererBitmap before = compositor.takeSnapshot();
  ASSERT_FALSE(before.empty());

  const auto& firstStats = compositor.lastFrameStats();
  const uint32_t totalLayers = static_cast<uint32_t>(compositor.layers().layers.size());
  EXPECT_EQ(firstStats.layersRasterized, totalLayers);

  // Apply a translation to the dragged entity's layer — no re-rasterization needed.
  bool found = compositor.setEntityLayerTransform(draggedEntity, Transformd::Translate(2.0, 3.0));
  EXPECT_TRUE(found);

  // Render again — no layers are dirty, so nothing is rasterized.
  compositor.renderFrame();
  const auto& secondStats = compositor.lastFrameStats();
  EXPECT_EQ(secondStats.layersRasterized, 0u);
  EXPECT_EQ(secondStats.layersReused, totalLayers);

  // The output should be different because the layer was translated.
  RendererBitmap after = compositor.takeSnapshot();
  ASSERT_FALSE(after.empty());
  EXPECT_GT(countDifferingPixels(before, after), 0)
      << "Translating a layer should produce different output without re-rasterization";
}

TEST_F(CompositedRendererTest, TransformOnlyCompositionResetToIdentity) {
  const std::string_view svg = R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="dragged" x="4" y="4" width="4" height="4" fill="red" />
  )svg";

  SVGDocument document = makeDocument(svg);
  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity draggedEntity = findRenderEntityById(document.registry(), "dragged");
  ASSERT_NE(draggedEntity, Entity(entt::null));

  compositor.prepare(document, {draggedEntity});
  compositor.renderFrame();
  RendererBitmap original = compositor.takeSnapshot();

  // Translate, render, then reset to identity, render again.
  compositor.setEntityLayerTransform(draggedEntity, Transformd::Translate(5.0, 5.0));
  compositor.renderFrame();

  compositor.setEntityLayerTransform(draggedEntity, Transformd());
  compositor.renderFrame();
  RendererBitmap restored = compositor.takeSnapshot();

  EXPECT_EQ(countDifferingPixels(original, restored), 0)
      << "Resetting composition transform to identity should restore original output";
}

TEST_F(CompositedRendererTest, SetLayerTransformById) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="item" x="4" y="4" width="4" height="4" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity itemEntity = findRenderEntityById(document.registry(), "item");
  ASSERT_NE(itemEntity, Entity(entt::null));

  compositor.prepare(document, {itemEntity});
  compositor.renderFrame();
  RendererBitmap before = compositor.takeSnapshot();

  // Find the layer and set transform by ID.
  auto layerId = compositor.findLayerForEntity(itemEntity);
  ASSERT_TRUE(layerId.has_value());

  compositor.setLayerTransform(*layerId, Transformd::Translate(3.0, 0.0));
  compositor.renderFrame();
  RendererBitmap after = compositor.takeSnapshot();

  EXPECT_GT(countDifferingPixels(before, after), 0);
}

// --- Phase 6: Content-sized layers ---

TEST_F(CompositedRendererTest, LayerBoundsAreContentSized) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="small" x="10" y="10" width="4" height="4" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity smallEntity = findRenderEntityById(document.registry(), "small");
  ASSERT_NE(smallEntity, Entity(entt::null));

  compositor.prepare(document, {smallEntity});

  // The dynamic layer containing the small rect should have bounds much smaller
  // than the full 16x16 canvas.
  bool foundSmallLayer = false;
  for (const auto& layer : compositor.layers().layers) {
    if (layer.reason == CompositingLayer::Reason::Animation) {
      EXPECT_LE(layer.bounds.width(), 6.0) << "Dynamic layer should be content-sized";
      EXPECT_LE(layer.bounds.height(), 6.0) << "Dynamic layer should be content-sized";
      EXPECT_GE(layer.bounds.topLeft.x, 9.0) << "Layer should be near x=10";
      EXPECT_GE(layer.bounds.topLeft.y, 9.0) << "Layer should be near y=10";
      foundSmallLayer = true;
    }
  }
  EXPECT_TRUE(foundSmallLayer);

  // Composited output should still match single-pass.
  SVGDocument document2 = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="small" x="10" y="10" width="4" height="4" fill="red" />
  )svg");
  RendererBitmap reference = renderSinglePass(document2);

  compositor.renderFrame();
  RendererBitmap composited = compositor.takeSnapshot();
  ASSERT_FALSE(composited.empty());
  EXPECT_EQ(countDifferingPixels(reference, composited), 0)
      << "Content-sized layers should still match single-pass";
}

// --- Phase 5: composeOnly, predictive rendering ---

TEST_F(CompositedRendererTest, ComposeOnlySkipsRasterization) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="item" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity itemEntity = findRenderEntityById(document.registry(), "item");
  ASSERT_NE(itemEntity, Entity(entt::null));

  compositor.prepare(document, {itemEntity});
  compositor.renderFrame();
  RendererBitmap baseline = compositor.takeSnapshot();

  // Mark a layer dirty but only compose — the dirty layer uses its stale pixmap.
  compositor.markLayerDirty(0);
  compositor.composeOnly();
  RendererBitmap composed = compositor.takeSnapshot();

  // Output should be identical to baseline (stale pixmap is the same content).
  EXPECT_EQ(countDifferingPixels(baseline, composed), 0);
}

TEST_F(CompositedRendererTest, ComposeOnlyWithTransformProducesDifferentOutput) {
  const std::string_view svg = R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="item" x="4" y="4" width="4" height="4" fill="red" />
  )svg";

  SVGDocument document = makeDocument(svg);
  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity itemEntity = findRenderEntityById(document.registry(), "item");
  ASSERT_NE(itemEntity, Entity(entt::null));

  compositor.prepare(document, {itemEntity});
  compositor.renderFrame();
  RendererBitmap before = compositor.takeSnapshot();

  // Apply transform and compose without rasterizing.
  compositor.setEntityLayerTransform(itemEntity, Transformd::Translate(3.0, 3.0));
  compositor.composeOnly();
  RendererBitmap after = compositor.takeSnapshot();

  EXPECT_GT(countDifferingPixels(before, after), 0)
      << "composeOnly with changed transform should produce different output";
}

TEST_F(CompositedRendererTest, RenderPredictedAndSwap) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="2" y="2" width="12" height="12" fill="green" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);
  compositor.prepare(document, {});

  // Initial render.
  compositor.renderFrame();
  RendererBitmap original = compositor.takeSnapshot();
  ASSERT_FALSE(original.empty());

  // Mark dirty and pre-render.
  compositor.markAllDirty();
  uint32_t predicted = compositor.renderPredicted();
  EXPECT_EQ(predicted, compositor.layers().layers.size());

  // The active cache is still stale (dirty), compose should use old pixmaps.
  compositor.composeOnly();
  RendererBitmap stale = compositor.takeSnapshot();
  EXPECT_EQ(countDifferingPixels(original, stale), 0)
      << "composeOnly after renderPredicted should still use the stale cache";

  // Swap predicted into active, then compose.
  compositor.swapPredicted();
  compositor.composeOnly();
  RendererBitmap fresh = compositor.takeSnapshot();
  EXPECT_EQ(countDifferingPixels(original, fresh), 0)
      << "After swapPredicted, composed output should match (same content)";

  // Layers should now be clean.
  compositor.renderFrame();
  EXPECT_EQ(compositor.lastFrameStats().layersRasterized, 0u);
}

TEST_F(CompositedRendererTest, RenderPredictedOnlyRenderssDirtyLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
    <rect x="6" y="6" width="4" height="4" fill="blue" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");
  ASSERT_NE(animatedEntity, Entity(entt::null));

  compositor.prepare(document, {animatedEntity});
  compositor.renderFrame();

  // Mark only the animated entity's layer dirty.
  compositor.markEntityDirty(animatedEntity);

  // Count how many layers are dirty.
  uint32_t predicted = compositor.renderPredicted();
  EXPECT_EQ(predicted, 1u) << "Only one layer should be pre-rendered";
}

// --- Phase 6: Pooling ---

TEST_F(CompositedRendererTest, OffscreenPoolReusesRenderers) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");
  ASSERT_NE(animatedEntity, Entity(entt::null));

  compositor.prepare(document, {animatedEntity});

  // First render: no pool hits (offscreen renderers created fresh).
  compositor.renderFrame();
  EXPECT_EQ(compositor.lastFrameStats().offscreenPoolHits, 0u);

  // Mark the animated layer dirty and render again.
  compositor.markEntityDirty(animatedEntity);
  compositor.renderFrame();

  // Second render should reuse the offscreen renderer for the dirty layer.
  EXPECT_GE(compositor.lastFrameStats().offscreenPoolHits, 1u);
}

TEST_F(CompositedRendererTest, ImageCacheReusedForCleanLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
    <rect x="12" y="12" width="4" height="4" fill="blue" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");
  ASSERT_NE(animatedEntity, Entity(entt::null));

  compositor.prepare(document, {animatedEntity});

  // First render: all layers rasterized, no image pool hits.
  compositor.renderFrame();
  EXPECT_EQ(compositor.lastFrameStats().imagePoolHits, 0u);

  // Mark only the animated layer dirty.
  compositor.markEntityDirty(animatedEntity);
  compositor.renderFrame();

  // Clean layers should reuse their cached ImageResource.
  const uint32_t totalLayers = static_cast<uint32_t>(compositor.layers().layers.size());
  EXPECT_GE(totalLayers, 3u) << "Should have at least 3 layers (static, dynamic, static)";
  // The number of image pool hits should equal the number of clean (non-dirty) layers.
  EXPECT_EQ(compositor.lastFrameStats().imagePoolHits, totalLayers - 1);
}

TEST_F(CompositedRendererTest, PooledRenderingStillMatchesSinglePass) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
    <rect x="12" y="12" width="4" height="4" fill="blue" />
  )svg");

  // Single-pass reference.
  auto refBackend = CreateActiveRendererInstance();
  CompositedRenderer refCompositor(*refBackend);
  refCompositor.prepare(document, {});
  refCompositor.renderFrame();
  RendererBitmap reference = refCompositor.takeSnapshot();

  // Composited with pooling — render twice to exercise pool reuse.
  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");

  compositor.prepare(document, {animatedEntity});
  compositor.renderFrame();
  compositor.markAllDirty();
  compositor.renderFrame();
  RendererBitmap pooled = compositor.takeSnapshot();

  EXPECT_EQ(countDifferingPixels(reference, pooled), 0)
      << "Pooled rendering should produce identical output to single-pass";
}

TEST_F(CompositedRendererTest, OpacityCompositionProducesDifferentOutput) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="target" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity targetEntity = findRenderEntityById(document.registry(), "target");
  ASSERT_NE(targetEntity, Entity(entt::null));

  compositor.prepare(document, {targetEntity});
  compositor.renderFrame();
  RendererBitmap opaque = compositor.takeSnapshot();

  // Apply 50% opacity to the target layer and compose without re-rasterization.
  compositor.setEntityLayerOpacity(targetEntity, 0.5);
  compositor.composeOnly();
  RendererBitmap halfOpacity = compositor.takeSnapshot();

  EXPECT_GT(countDifferingPixels(opaque, halfOpacity), 0)
      << "Opacity change should produce visibly different output";
}

TEST_F(CompositedRendererTest, SetLayerOpacityById) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="target" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity targetEntity = findRenderEntityById(document.registry(), "target");

  compositor.prepare(document, {targetEntity});
  compositor.renderFrame();
  RendererBitmap opaque = compositor.takeSnapshot();

  // Find layer and set opacity directly.
  auto layerId = compositor.findLayerForEntity(targetEntity);
  ASSERT_TRUE(layerId.has_value());
  compositor.setLayerOpacity(*layerId, 0.0);
  compositor.composeOnly();
  RendererBitmap transparent = compositor.takeSnapshot();

  EXPECT_GT(countDifferingPixels(opaque, transparent), 0)
      << "Zero opacity should hide the layer entirely";
}

TEST_F(CompositedRendererTest, OpacityDoesNotRequireReRasterization) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="target" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity targetEntity = findRenderEntityById(document.registry(), "target");

  compositor.prepare(document, {targetEntity});
  compositor.renderFrame();

  // Change opacity and render again. No layers should be rasterized.
  compositor.setEntityLayerOpacity(targetEntity, 0.75);
  compositor.renderFrame();
  EXPECT_EQ(compositor.lastFrameStats().layersRasterized, 0u)
      << "Opacity change should not cause re-rasterization";
}

TEST_F(CompositedRendererTest, IncrementalPreparePreservesCleanLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
    <rect x="12" y="12" width="4" height="4" fill="blue" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");
  ASSERT_NE(animatedEntity, Entity(entt::null));

  compositor.prepare(document, {animatedEntity});
  compositor.renderFrame();

  // All layers should have been rasterized on first render.
  const uint32_t totalLayers = static_cast<uint32_t>(compositor.layers().layers.size());
  EXPECT_EQ(compositor.lastFrameStats().layersRasterized, totalLayers);

  // Call prepare again with the same promotion set — incremental path.
  compositor.prepare(document, {animatedEntity});
  compositor.renderFrame();

  // Since bounds haven't changed, all layers should be reused (none rasterized).
  EXPECT_EQ(compositor.lastFrameStats().layersRasterized, 0u)
      << "Incremental prepare with same promotion set should preserve clean layers";
  EXPECT_EQ(compositor.lastFrameStats().layersReused, totalLayers);
}

TEST_F(CompositedRendererTest, IncrementalPrepareStillMatchesSinglePass) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  // Single-pass reference.
  auto refBackend = CreateActiveRendererInstance();
  CompositedRenderer refCompositor(*refBackend);
  refCompositor.prepare(document, {});
  refCompositor.renderFrame();
  RendererBitmap reference = refCompositor.takeSnapshot();

  // Composited with incremental prepare.
  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity animatedEntity = findRenderEntityById(document.registry(), "animated");

  compositor.prepare(document, {animatedEntity});
  compositor.renderFrame();

  // Incremental prepare with same set.
  compositor.prepare(document, {animatedEntity});
  compositor.composeOnly();
  RendererBitmap incremental = compositor.takeSnapshot();

  EXPECT_EQ(countDifferingPixels(reference, incremental), 0)
      << "Incremental prepare output should match single-pass";
}

TEST_F(CompositedRendererTest, ChangedPromotionSetTriggersFullRebuild) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="a" x="0" y="0" width="8" height="8" fill="red" />
    <rect id="b" x="8" y="8" width="8" height="8" fill="blue" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);

  RendererUtils::prepareDocumentForRendering(document, false);
  Entity entityA = findRenderEntityById(document.registry(), "a");
  Entity entityB = findRenderEntityById(document.registry(), "b");

  // Prepare with entity A promoted.
  compositor.prepare(document, {entityA});
  compositor.renderFrame();
  const uint32_t layersWithA = static_cast<uint32_t>(compositor.layers().layers.size());

  // Prepare with entity B promoted — different set, full rebuild.
  compositor.prepare(document, {entityB});
  compositor.renderFrame();

  // All layers should be rasterized (full rebuild).
  const uint32_t layersWithB = static_cast<uint32_t>(compositor.layers().layers.size());
  EXPECT_EQ(compositor.lastFrameStats().layersRasterized, layersWithB)
      << "Changed promotion set should trigger full rebuild with all layers rasterized";
  EXPECT_GE(layersWithA, 2u);
  EXPECT_GE(layersWithB, 2u);
}

TEST_F(CompositedRendererTest, SkipsCompositionWhenNothingChanged) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="0" y="0" width="16" height="16" fill="white" />
    <rect x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  auto backend = CreateActiveRendererInstance();
  CompositedRenderer compositor(*backend);
  compositor.prepare(document, {});

  // First render: rasterizes and composes.
  compositor.renderFrame();
  RendererBitmap first = compositor.takeSnapshot();
  ASSERT_FALSE(first.empty());

  // Second render: nothing changed, composition should be skipped.
  // The output should still be identical (renderer preserves its surface).
  compositor.renderFrame();
  RendererBitmap second = compositor.takeSnapshot();

  EXPECT_EQ(countDifferingPixels(first, second), 0)
      << "Repeated renderFrame with no changes should produce identical output";
  EXPECT_EQ(compositor.lastFrameStats().layersRasterized, 0u);
  EXPECT_EQ(compositor.lastFrameStats().layersReused,
            static_cast<uint32_t>(compositor.layers().layers.size()));
}

}  // namespace
}  // namespace donner::svg
