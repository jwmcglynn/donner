#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGUnknownElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

namespace {

using components::DirtyFlagsComponent;
using components::RenderTreeState;

class SVGInvalidationTests : public testing::Test {
protected:
  SVGInvalidationTests() { document_.setCanvasSize(800, 600); }

  SVGDocument parseSVG(std::string_view input) {
    parser::SVGParser::Options options;
    options.parseAsInlineSVG = true;

    auto maybeResult = parser::SVGParser::ParseSVG(input, nullptr, options);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  /// Helper to clear dirty flags on an element.
  static void clearDirty(SVGElement element) {
    element.entityHandle().remove<DirtyFlagsComponent>();
  }

  /// Helper to check whether an element has specific dirty flags set.
  static bool hasDirtyFlags(SVGElement element, uint16_t flags) {
    const auto* dirty = element.entityHandle().try_get<DirtyFlagsComponent>();
    return dirty != nullptr && dirty->test(flags);
  }

  /// Helper to check whether an element has any dirty flags at all.
  static bool isDirty(SVGElement element) {
    const auto* dirty = element.entityHandle().try_get<DirtyFlagsComponent>();
    return dirty != nullptr && dirty->flags != DirtyFlagsComponent::None;
  }

  /// Simulate a completed first render by setting RenderTreeState appropriately.
  static void simulateRenderComplete(SVGDocument& doc) {
    auto& registry = doc.registry();
    if (!registry.ctx().contains<RenderTreeState>()) {
      registry.ctx().emplace<RenderTreeState>();
    }
    auto& renderState = registry.ctx().get<RenderTreeState>();
    renderState.hasBeenBuilt = true;
    renderState.needsFullRebuild = false;
    renderState.needsFullStyleRecompute = false;

    // Clear all per-entity dirty flags.
    auto view = registry.view<DirtyFlagsComponent>();
    for (auto entity : view) {
      registry.remove<DirtyFlagsComponent>(entity);
    }
  }

  SVGDocument document_;
};

// ---------------------------------------------------------------------------
// setStyle
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, SetStyleMarksDirtyWithStyleCascade) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setStyle("fill: red");

  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Paint));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::RenderInstance));
  // StyleCascade is Style | Paint | Filter | RenderInstance.
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::StyleCascade));
}

TEST_F(SVGInvalidationTests, SetStyleDoesNotMarkTransformDirty) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setStyle("fill: blue");

  // A fill change should not set Transform or Layout flags.
  EXPECT_FALSE(hasDirtyFlags(*target, DirtyFlagsComponent::Transform));
  EXPECT_FALSE(hasDirtyFlags(*target, DirtyFlagsComponent::Layout));
}

// ---------------------------------------------------------------------------
// setClassName
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, SetClassNameMarksDirtyWithStyleCascade) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setClassName("highlight");

  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::StyleCascade));
}

TEST_F(SVGInvalidationTests, SetClassNameRequestsFullStyleRecompute) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setClassName("highlight");

  auto& renderState = doc.registry().ctx().get<RenderTreeState>();
  EXPECT_TRUE(renderState.needsFullStyleRecompute);
}

// ---------------------------------------------------------------------------
// setAttribute (presentation attribute and generic attribute)
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, SetAttributePresentationAttributeMarksDirty) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setAttribute("fill", "green");

  // Presentation attributes mark StyleCascade and Shape dirty.
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Shape));
}

TEST_F(SVGInvalidationTests, SetAttributeTransformMarksDirtyTransform) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setAttribute("transform", "translate(10 20)");

  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Transform));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::WorldTransform));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::RenderInstance));
}

TEST_F(SVGInvalidationTests, SetAttributeGenericRequestsFullStyleRecompute) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  // A non-presentation, non-special attribute still requests full style recompute because
  // it may affect CSS attribute selectors.
  target->setAttribute("data-custom", "value");

  auto& renderState = doc.registry().ctx().get<RenderTreeState>();
  EXPECT_TRUE(renderState.needsFullStyleRecompute);
}

// ---------------------------------------------------------------------------
// appendChild
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, AppendChildMarksParentDirtyAll) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"></g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());

  auto newChild = SVGRectElement::Create(doc);
  parent->appendChild(newChild);

  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
  EXPECT_TRUE(hasDirtyFlags(newChild, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, AppendChildSetsNeedsFullRebuild) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"></g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());

  auto newChild = SVGRectElement::Create(doc);
  parent->appendChild(newChild);

  auto& renderState = doc.registry().ctx().get<RenderTreeState>();
  EXPECT_TRUE(renderState.needsFullRebuild);
}

// ---------------------------------------------------------------------------
// removeChild
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, RemoveChildMarksParentDirtyAll) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  parent->removeChild(*child);

  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
}

TEST_F(SVGInvalidationTests, RemoveChildSetsNeedsFullRebuild) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  parent->removeChild(*child);

  auto& renderState = doc.registry().ctx().get<RenderTreeState>();
  EXPECT_TRUE(renderState.needsFullRebuild);
}

// ---------------------------------------------------------------------------
// Dirty propagation to descendants
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, SetStylePropagatesDirtyToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
        <g id="nested">
          <rect id="grandchild" width="5" height="5" />
        </g>
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  auto grandchild = doc.querySelector("#grandchild");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(grandchild.has_value());

  parent->setStyle("opacity: 0.5");

  // The parent itself gets StyleCascade.
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::Style));

  // Descendants should get style-related dirty flags propagated.
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Paint));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::RenderInstance));

  EXPECT_TRUE(hasDirtyFlags(*grandchild, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*grandchild, DirtyFlagsComponent::Paint));
  EXPECT_TRUE(hasDirtyFlags(*grandchild, DirtyFlagsComponent::RenderInstance));
}

TEST_F(SVGInvalidationTests, SetTransformPropagatesWorldTransformToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  parent->setAttribute("transform", "scale(2)");

  // Parent gets Transform + WorldTransform + RenderInstance.
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::Transform));
  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::WorldTransform));

  // Child should get WorldTransform + RenderInstance propagated.
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::WorldTransform));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::RenderInstance));
}

TEST_F(SVGInvalidationTests, SetClassNamePropagatesDirtyToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  parent->setClassName("highlight");

  // Style dirty flags should propagate to descendants.
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Paint));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::RenderInstance));
}

TEST_F(SVGInvalidationTests, AppendChildPropagatesDirtyToInsertedSubtree) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent"></g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  ASSERT_TRUE(parent.has_value());

  // Build a subtree: g > rect
  auto newGroup = SVGUnknownElement::Create(doc, "g");
  auto newRect = SVGRectElement::Create(doc);
  newGroup.appendChild(newRect);

  // Clear dirty flags from creating the subtree.
  clearDirty(newGroup);
  clearDirty(newRect);

  parent->appendChild(newGroup);

  // Both the new group and its child rect should be dirty.
  EXPECT_TRUE(hasDirtyFlags(newGroup, DirtyFlagsComponent::All));
  EXPECT_TRUE(hasDirtyFlags(newRect, DirtyFlagsComponent::All));
}

// ---------------------------------------------------------------------------
// Clean state after simulated render
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, DirtyFlagsClearedAfterSimulatedRender) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  // After parsing, elements may have dirty flags.
  // Simulate a completed render which clears them.
  simulateRenderComplete(doc);

  EXPECT_FALSE(isDirty(*target));
}

TEST_F(SVGInvalidationTests, MutationAfterRenderSetsDirtyAgain) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EXPECT_FALSE(isDirty(*target));

  // Mutate after render.
  target->setStyle("stroke: blue");

  // Should be dirty again.
  EXPECT_TRUE(isDirty(*target));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::StyleCascade));
}

TEST_F(SVGInvalidationTests, MultipleMutationsAccumulateFlags) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" />
    </svg>
  )");
  simulateRenderComplete(doc);

  auto target = doc.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  target->setStyle("fill: red");
  target->setAttribute("transform", "rotate(45)");

  // Should have both style and transform flags accumulated.
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::Transform));
  EXPECT_TRUE(hasDirtyFlags(*target, DirtyFlagsComponent::WorldTransform));
}

// ---------------------------------------------------------------------------
// remove() (self-removal) marks parent dirty
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, RemoveSelfMarksParentDirty) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  child->remove();

  EXPECT_TRUE(hasDirtyFlags(*parent, DirtyFlagsComponent::All));
}

// ---------------------------------------------------------------------------
// Inherited presentation attribute propagation
// ---------------------------------------------------------------------------

TEST_F(SVGInvalidationTests, InheritedPresentationAttributePropagatesDirtyToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");
  simulateRenderComplete(doc);

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  // "fill" is an inherited presentation attribute.
  parent->setAttribute("fill", "red");

  // Child should get style-related dirty flags because fill is inherited.
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Style));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::Paint));
  EXPECT_TRUE(hasDirtyFlags(*child, DirtyFlagsComponent::RenderInstance));
}

}  // namespace

}  // namespace donner::svg
