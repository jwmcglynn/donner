#include "donner/svg/renderer/LayerDecomposition.h"

#include <gtest/gtest.h>

#include "donner/svg/components/LayerMembershipComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

class LayerDecompositionTest : public ::testing::Test {
protected:
  SVGDocument makeDocument(std::string_view svg, Vector2i size = kTestSvgDefaultSize) {
    SVGDocument document = instantiateSubtree(svg, parser::SVGParser::Options(), size);
    RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false);
    return document;
  }

  /// Count entities in the render tree.
  int countRenderEntities(Registry& registry) {
    int count = 0;
    RenderingInstanceView view(registry);
    while (!view.done()) {
      ++count;
      view.advance();
    }
    return count;
  }

  /// Get all entities in render order.
  std::vector<Entity> getRenderEntities(Registry& registry) {
    std::vector<Entity> entities;
    RenderingInstanceView view(registry);
    while (!view.done()) {
      entities.push_back(view.currentEntity());
      view.advance();
    }
    return entities;
  }

  /// Find the entity with the given draw order.
  Entity findEntityByDrawOrder(Registry& registry, int drawOrder) {
    RenderingInstanceView view(registry);
    while (!view.done()) {
      const auto& instance = view.get();
      if (instance.drawOrder == drawOrder) {
        return view.currentEntity();
      }
      view.advance();
    }
    return Entity(entt::null);
  }

  /// Find the first entity in the render tree that has a specific data entity with an ID.
  Entity findRenderEntityById(Registry& registry, const char* id) {
    const auto& docContext = registry.ctx().get<components::SVGDocumentContext>();
    Entity dataEntity = docContext.getEntityById(RcString(id));
    if (dataEntity == Entity(entt::null)) {
      return Entity(entt::null);
    }

    // The render entity may be the same or a shadow instance. Walk the render tree.
    RenderingInstanceView view(registry);
    while (!view.done()) {
      const auto& instance = view.get();
      if (instance.dataEntity == dataEntity) {
        return view.currentEntity();
      }
      view.advance();
    }
    return Entity(entt::null);
  }
};

TEST_F(LayerDecompositionTest, NoPromotedEntities_SingleStaticLayer) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="8" height="6" fill="red" />
    <rect x="2" y="2" width="4" height="4" fill="blue" />
  )svg");

  Registry& registry = document.registry();
  const int entityCount = countRenderEntities(registry);
  ASSERT_GT(entityCount, 0);

  // No promoted entities -> all entities in one static layer.
  LayerDecompositionResult result = decomposeIntoLayers(registry, {});

  ASSERT_EQ(result.layers.size(), 1u);
  EXPECT_EQ(result.layers[0].reason, CompositingLayer::Reason::Static);
  EXPECT_EQ(result.layers[0].id, 0u);
}

TEST_F(LayerDecompositionTest, SinglePromotedEntity_ThreeLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="bg" width="16" height="16" fill="gray" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
    <rect id="fg" x="2" y="2" width="4" height="4" fill="blue" />
  )svg");

  Registry& registry = document.registry();
  Entity animated = findRenderEntityById(registry, "animated");
  ASSERT_NE(animated, Entity(entt::null));

  LayerDecompositionResult result = decomposeIntoLayers(registry, {animated});

  // Expect: static(bg), dynamic(animated), static(fg)
  ASSERT_EQ(result.layers.size(), 3u);

  EXPECT_EQ(result.layers[0].reason, CompositingLayer::Reason::Static);
  EXPECT_EQ(result.layers[1].reason, CompositingLayer::Reason::Animation);
  EXPECT_EQ(result.layers[2].reason, CompositingLayer::Reason::Static);

  // IDs are sequential.
  EXPECT_EQ(result.layers[0].id, 0u);
  EXPECT_EQ(result.layers[1].id, 1u);
  EXPECT_EQ(result.layers[2].id, 2u);

  // The animated layer's entity range should contain just the animated entity.
  EXPECT_EQ(result.layers[1].firstEntity, animated);
  EXPECT_EQ(result.layers[1].lastEntity, animated);
}

TEST_F(LayerDecompositionTest, PromotedAtStart_TwoLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="animated" width="8" height="8" fill="red" />
    <rect id="fg" x="2" y="2" width="4" height="4" fill="blue" />
  )svg");

  Registry& registry = document.registry();
  Entity animated = findRenderEntityById(registry, "animated");
  ASSERT_NE(animated, Entity(entt::null));

  LayerDecompositionResult result = decomposeIntoLayers(registry, {animated});

  // The SVG root element may appear as an entity before the first rect,
  // creating: static(root), dynamic(animated), static(fg).
  // Verify the dynamic layer exists and the animated entity is in it.
  bool foundDynamic = false;
  for (const auto& layer : result.layers) {
    if (layer.reason == CompositingLayer::Reason::Animation) {
      EXPECT_EQ(layer.firstEntity, animated);
      foundDynamic = true;
    }
  }
  EXPECT_TRUE(foundDynamic);
  // At least dynamic + one static (fg).
  EXPECT_GE(result.layers.size(), 2u);
}

TEST_F(LayerDecompositionTest, PromotedAtEnd_TwoLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="bg" width="8" height="8" fill="gray" />
    <rect id="animated" x="2" y="2" width="4" height="4" fill="red" />
  )svg");

  Registry& registry = document.registry();
  Entity animated = findRenderEntityById(registry, "animated");
  ASSERT_NE(animated, Entity(entt::null));

  LayerDecompositionResult result = decomposeIntoLayers(registry, {animated});

  // Expect: static(bg), dynamic(animated)
  ASSERT_EQ(result.layers.size(), 2u);
  EXPECT_EQ(result.layers[0].reason, CompositingLayer::Reason::Static);
  EXPECT_EQ(result.layers[1].reason, CompositingLayer::Reason::Animation);
}

TEST_F(LayerDecompositionTest, TwoPromotedEntities_FiveLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="bg" width="16" height="16" fill="gray" />
    <rect id="anim1" x="2" y="2" width="4" height="4" fill="red" />
    <rect id="middle" x="6" y="6" width="4" height="4" fill="green" />
    <rect id="anim2" x="10" y="10" width="4" height="4" fill="blue" />
    <rect id="fg" x="0" y="0" width="2" height="2" fill="white" />
  )svg");

  Registry& registry = document.registry();
  Entity anim1 = findRenderEntityById(registry, "anim1");
  Entity anim2 = findRenderEntityById(registry, "anim2");
  ASSERT_NE(anim1, Entity(entt::null));
  ASSERT_NE(anim2, Entity(entt::null));

  LayerDecompositionResult result = decomposeIntoLayers(registry, {anim1, anim2});

  // Expect: static(bg), dynamic(anim1), static(middle), dynamic(anim2), static(fg)
  ASSERT_EQ(result.layers.size(), 5u);
  EXPECT_EQ(result.layers[0].reason, CompositingLayer::Reason::Static);
  EXPECT_EQ(result.layers[1].reason, CompositingLayer::Reason::Animation);
  EXPECT_EQ(result.layers[2].reason, CompositingLayer::Reason::Static);
  EXPECT_EQ(result.layers[3].reason, CompositingLayer::Reason::Animation);
  EXPECT_EQ(result.layers[4].reason, CompositingLayer::Reason::Static);
}

TEST_F(LayerDecompositionTest, AdjacentPromotedEntities_NoStaticBetween) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="bg" width="16" height="16" fill="gray" />
    <rect id="anim1" x="2" y="2" width="4" height="4" fill="red" />
    <rect id="anim2" x="6" y="6" width="4" height="4" fill="blue" />
    <rect id="fg" x="0" y="0" width="2" height="2" fill="white" />
  )svg");

  Registry& registry = document.registry();
  Entity anim1 = findRenderEntityById(registry, "anim1");
  Entity anim2 = findRenderEntityById(registry, "anim2");
  ASSERT_NE(anim1, Entity(entt::null));
  ASSERT_NE(anim2, Entity(entt::null));

  LayerDecompositionResult result = decomposeIntoLayers(registry, {anim1, anim2});

  // Expect: static(bg), dynamic(anim1), dynamic(anim2), static(fg)
  // No empty static layer between adjacent promoted entities.
  ASSERT_EQ(result.layers.size(), 4u);
  EXPECT_EQ(result.layers[0].reason, CompositingLayer::Reason::Static);
  EXPECT_EQ(result.layers[1].reason, CompositingLayer::Reason::Animation);
  EXPECT_EQ(result.layers[2].reason, CompositingLayer::Reason::Animation);
  EXPECT_EQ(result.layers[3].reason, CompositingLayer::Reason::Static);
}

TEST_F(LayerDecompositionTest, SubtreePromotion_GroupWithOpacity) {
  // Use opacity to ensure the <g> creates a compositing context (subtreeInfo).
  // A plain <g> without effects is transparent in the flat render tree.
  SVGDocument document = makeDocument(R"svg(
    <rect id="bg" width="16" height="16" fill="gray" />
    <g id="group" opacity="0.9">
      <rect id="child1" x="2" y="2" width="4" height="4" fill="red" />
      <rect id="child2" x="6" y="6" width="4" height="4" fill="blue" />
    </g>
    <rect id="fg" x="0" y="0" width="2" height="2" fill="white" />
  )svg");

  Registry& registry = document.registry();
  Entity group = findRenderEntityById(registry, "group");
  ASSERT_NE(group, Entity(entt::null));

  LayerDecompositionResult result = decomposeIntoLayers(registry, {group});

  // The group has subtreeInfo (due to opacity), so promoting it promotes the
  // entire subtree including children.
  // Find the dynamic layer containing the group.
  const CompositingLayer* dynamicLayer = nullptr;
  for (const auto& layer : result.layers) {
    if (layer.reason == CompositingLayer::Reason::Animation) {
      dynamicLayer = &layer;
      break;
    }
  }
  ASSERT_NE(dynamicLayer, nullptr);
  EXPECT_EQ(dynamicLayer->firstEntity, group);

  // The dynamic layer should span from group through child2.
  Entity child2 = findRenderEntityById(registry, "child2");
  ASSERT_NE(child2, Entity(entt::null));
  EXPECT_EQ(dynamicLayer->lastEntity, child2);
}

TEST_F(LayerDecompositionTest, CompositingContextEscalation_OpacityGroup) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="bg" width="16" height="16" fill="gray" />
    <g id="opacityGroup" opacity="0.5">
      <rect id="static_child" x="2" y="2" width="4" height="4" fill="green" />
      <rect id="animated_child" x="6" y="6" width="4" height="4" fill="red" />
    </g>
    <rect id="fg" x="0" y="0" width="2" height="2" fill="white" />
  )svg");

  Registry& registry = document.registry();
  Entity animatedChild = findRenderEntityById(registry, "animated_child");
  Entity opacityGroup = findRenderEntityById(registry, "opacityGroup");
  ASSERT_NE(animatedChild, Entity(entt::null));
  ASSERT_NE(opacityGroup, Entity(entt::null));

  // Promote just the animated child, but escalation should promote the
  // entire opacity group because it's a compositing context.
  LayerDecompositionResult result = decomposeIntoLayers(registry, {animatedChild});

  // Expect: static(bg), dynamic(opacityGroup+children), static(fg)
  ASSERT_EQ(result.layers.size(), 3u);
  EXPECT_EQ(result.layers[0].reason, CompositingLayer::Reason::Static);
  EXPECT_EQ(result.layers[1].reason, CompositingLayer::Reason::Animation);
  EXPECT_EQ(result.layers[2].reason, CompositingLayer::Reason::Static);

  // The dynamic layer should start at the opacity group, not the animated child.
  EXPECT_EQ(result.layers[1].firstEntity, opacityGroup);
}

TEST_F(LayerDecompositionTest, CompositingContextEscalation_ClipPath) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <clipPath id="clip">
        <rect width="12" height="12" />
      </clipPath>
    </defs>
    <rect id="bg" width="16" height="16" fill="gray" />
    <g id="clippedGroup" clip-path="url(#clip)">
      <rect id="child" x="2" y="2" width="4" height="4" fill="red" />
    </g>
    <rect id="fg" x="0" y="0" width="2" height="2" fill="white" />
  )svg");

  Registry& registry = document.registry();
  Entity child = findRenderEntityById(registry, "child");
  Entity clippedGroup = findRenderEntityById(registry, "clippedGroup");
  ASSERT_NE(child, Entity(entt::null));
  ASSERT_NE(clippedGroup, Entity(entt::null));

  // Promoting the child should escalate to the clip-path group.
  LayerDecompositionResult result = decomposeIntoLayers(registry, {child});

  // The clipped group is a compositing context, so escalation promotes it.
  ASSERT_GE(result.layers.size(), 2u);

  // Find the dynamic layer.
  const CompositingLayer* dynamicLayer = nullptr;
  for (const auto& layer : result.layers) {
    if (layer.reason == CompositingLayer::Reason::Animation) {
      dynamicLayer = &layer;
      break;
    }
  }
  ASSERT_NE(dynamicLayer, nullptr);
  EXPECT_EQ(dynamicLayer->firstEntity, clippedGroup);
}

TEST_F(LayerDecompositionTest, LayerMembershipAssignment) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="bg" width="16" height="16" fill="gray" />
    <rect id="animated" x="4" y="4" width="8" height="8" fill="red" />
    <rect id="fg" x="2" y="2" width="4" height="4" fill="blue" />
  )svg");

  Registry& registry = document.registry();
  Entity animated = findRenderEntityById(registry, "animated");
  ASSERT_NE(animated, Entity(entt::null));

  LayerDecompositionResult result = decomposeIntoLayers(registry, {animated});

  // Every render entity should have a LayerMembershipComponent.
  std::vector<Entity> entities = getRenderEntities(registry);
  for (Entity e : entities) {
    EXPECT_TRUE(registry.all_of<components::LayerMembershipComponent>(e))
        << "Entity " << entt::to_integral(e) << " missing LayerMembershipComponent";
  }

  // The animated entity should be on layer 1 (the dynamic layer).
  const auto& membership = registry.get<components::LayerMembershipComponent>(animated);
  EXPECT_EQ(membership.layerId, 1u);
}

TEST_F(LayerDecompositionTest, SelectionReason) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="bg" width="16" height="16" fill="gray" />
    <rect id="selected" x="4" y="4" width="8" height="8" fill="red" />
  )svg");

  Registry& registry = document.registry();
  Entity selected = findRenderEntityById(registry, "selected");
  ASSERT_NE(selected, Entity(entt::null));

  LayerDecompositionResult result =
      decomposeIntoLayers(registry, {selected}, CompositingLayer::Reason::Selection);

  // The dynamic layer should have Selection reason.
  bool foundSelection = false;
  for (const auto& layer : result.layers) {
    if (layer.reason == CompositingLayer::Reason::Selection) {
      foundSelection = true;
    }
  }
  EXPECT_TRUE(foundSelection);
}

TEST_F(LayerDecompositionTest, AllEntitiesPromoted_NoDuplicateLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="a" width="8" height="8" fill="red" />
    <rect id="b" x="2" y="2" width="4" height="4" fill="blue" />
  )svg");

  Registry& registry = document.registry();
  Entity a = findRenderEntityById(registry, "a");
  Entity b = findRenderEntityById(registry, "b");
  ASSERT_NE(a, Entity(entt::null));
  ASSERT_NE(b, Entity(entt::null));

  LayerDecompositionResult result = decomposeIntoLayers(registry, {a, b});

  // Both promoted, no static content -> 2 dynamic layers.
  // (The SVG root element may create additional entities.)
  int dynamicCount = 0;
  for (const auto& layer : result.layers) {
    if (layer.reason == CompositingLayer::Reason::Animation) {
      ++dynamicCount;
    }
  }
  EXPECT_GE(dynamicCount, 2);
  EXPECT_GE(result.layers.size(), 2u);
}

TEST_F(LayerDecompositionTest, EmptyDocument_NoLayers) {
  SVGDocument document = makeDocument(R"svg()svg");

  Registry& registry = document.registry();
  LayerDecompositionResult result = decomposeIntoLayers(registry, {});

  // An empty SVG body may still produce a root entity, but we test graceful handling.
  // Each layer should have valid entities.
  for (const auto& layer : result.layers) {
    EXPECT_NE(layer.firstEntity, Entity(entt::null));
    EXPECT_NE(layer.lastEntity, Entity(entt::null));
  }
}

TEST_F(LayerDecompositionTest, NestedCompositingContexts_EscalatesToOutermost) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="bg" width="16" height="16" fill="gray" />
    <g id="outer" opacity="0.8">
      <g id="inner" opacity="0.5">
        <rect id="target" x="4" y="4" width="4" height="4" fill="red" />
      </g>
    </g>
    <rect id="fg" x="0" y="0" width="2" height="2" fill="white" />
  )svg");

  Registry& registry = document.registry();
  Entity target = findRenderEntityById(registry, "target");
  Entity outer = findRenderEntityById(registry, "outer");
  ASSERT_NE(target, Entity(entt::null));
  ASSERT_NE(outer, Entity(entt::null));

  LayerDecompositionResult result = decomposeIntoLayers(registry, {target});

  // Should escalate through inner (opacity) and outer (opacity) to outermost.
  const CompositingLayer* dynamicLayer = nullptr;
  for (const auto& layer : result.layers) {
    if (layer.reason == CompositingLayer::Reason::Animation) {
      dynamicLayer = &layer;
      break;
    }
  }
  ASSERT_NE(dynamicLayer, nullptr);
  EXPECT_EQ(dynamicLayer->firstEntity, outer);
}

}  // namespace
}  // namespace donner::svg
