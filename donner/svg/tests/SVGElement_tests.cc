#include "donner/svg/SVGElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <deque>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGUnknownElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PropertyRegistry.h"

using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::Optional;

namespace donner::svg {

namespace {

class SVGElementTests : public testing::Test {
protected:
  SVGElementTests() { document_.setCanvasSize(800, 600); }

  /// Creates a \ref SVGUnknownElement element with typeString "unknown"
  SVGGraphicsElement create() { return SVGUnknownElement::Create(document_, "unknown"); }

  /// Create an element with the given ID.
  SVGGraphicsElement createWithId(std::string_view id) {
    SVGGraphicsElement result = create();
    result.setId(id);
    return result;
  }

  /// Create an SVGRectElement.
  SVGRectElement createRect() { return SVGRectElement::Create(document_); }

  std::vector<SVGElement> children(const SVGElement& element) {
    std::vector<SVGElement> result;
    for (auto cur = element.firstChild(); cur; cur = cur->nextSibling()) {
      result.push_back(cur.value());
    }

    // Iterate in reverse order and verify.
    if (result.empty()) {
      EXPECT_FALSE(element.lastChild().has_value());
    } else {
      std::deque<SVGElement> resultReverse;
      for (auto cur = element.lastChild(); cur; cur = cur->previousSibling()) {
        resultReverse.push_front(cur.value());
      }

      EXPECT_THAT(resultReverse, ElementsAreArray(result));
    }

    return result;
  }

  SVGDocument parseSVG(std::string_view input) {
    parser::SVGParser::Options options;
    options.parseAsInlineSVG = true;

    auto maybeResult = parser::SVGParser::ParseSVG(input, nullptr, options);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  SVGDocument document_;
};

MATCHER_P(ElementIdEq, id, "") {
  return arg.id() == id;
}

}  // namespace

TEST_F(SVGElementTests, Equality) {
  SVGElement element1 = create();
  SVGElement element2 = create();
  EXPECT_EQ(element1, element1);
  EXPECT_EQ(element2, element2);
  EXPECT_NE(element1, element2);
}

TEST_F(SVGElementTests, Assign) {
  SVGElement element1 = create();
  SVGElement element2 = create();
  SVGElement element3 = create();
  EXPECT_NE(element1, element2);
  EXPECT_NE(element1, element3);

  element1 = element2;
  EXPECT_EQ(element1, element2);

  // Now test with a move.
  element3 = std::move(element2);
  EXPECT_EQ(element1, element3);
  EXPECT_NE(element2, element3);  // element2 should be invalid.
}

TEST_F(SVGElementTests, CastRect) {
  // Parse a simple SVG with a single rect
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="myRect" x="10" y="10" width="100" height="100" />
    </svg>
  )");

  // Ensure we have a result
  auto maybeRect = doc.querySelector("#myRect");
  ASSERT_TRUE(maybeRect.has_value());

  auto element = maybeRect.value();

  // Check isa<> for multiple types
  EXPECT_TRUE(element.isa<SVGElement>());
  EXPECT_TRUE(element.isa<SVGRectElement>());
  EXPECT_FALSE(element.isa<SVGGElement>());
  EXPECT_FALSE(element.isa<SVGUnknownElement>());

  // tryCast<>() should succeed for SVGRectElement
  {
    auto rectOptional = element.tryCast<SVGRectElement>();
    EXPECT_TRUE(rectOptional.has_value());
  }

  // tryCast<>() should fail for SVGGElement
  {
    auto gOptional = element.tryCast<SVGGElement>();
    EXPECT_FALSE(gOptional.has_value());
  }

  // cast<>() should succeed for SVGRectElement
  {
    // If the cast is not correct, an assertion will fail in debug builds.
    [[maybe_unused]] auto rectElement = element.cast<SVGRectElement>();
  }
}

TEST_F(SVGElementTests, CastGroup) {
  // Parse a simple SVG with a single group
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="myGroup"></g>
    </svg>
  )");

  // Ensure we have a result
  auto maybeGroup = doc.querySelector("#myGroup");
  ASSERT_TRUE(maybeGroup.has_value());

  auto element = maybeGroup.value();

  // Check isa<> for multiple types
  EXPECT_TRUE(element.isa<SVGElement>());
  EXPECT_TRUE(element.isa<SVGGElement>());
  EXPECT_FALSE(element.isa<SVGRectElement>());
  EXPECT_FALSE(element.isa<SVGUnknownElement>());

  // tryCast<>() should succeed for SVGGElement
  {
    auto gOptional = element.tryCast<SVGGElement>();
    EXPECT_TRUE(gOptional.has_value());
  }

  // tryCast<>() should fail for SVGRectElement
  {
    auto rectOptional = element.tryCast<SVGRectElement>();
    EXPECT_FALSE(rectOptional.has_value());
  }

  // cast<>() should succeed for SVGGElement
  [[maybe_unused]] auto groupElement = element.cast<SVGGElement>();
}

TEST_F(SVGElementTests, CastUnknown) {
  // create() returns an SVGUnknownElement::Create(document_, "unknown")
  auto element = create();
  EXPECT_EQ(element.type(), ElementType::Unknown);

  // This is definitely an SVGElement
  EXPECT_TRUE(element.isa<SVGElement>());

  // Should be recognized as an unknown
  EXPECT_TRUE(element.isa<SVGUnknownElement>());

  // A direct cast to unknown should succeed
  {
    auto unknownOpt = element.tryCast<SVGUnknownElement>();
    EXPECT_TRUE(unknownOpt.has_value());
    // Or do the hard cast
    [[maybe_unused]] auto unknownElem = element.cast<SVGUnknownElement>();
  }

  // But it's not a rect or group
  {
    auto rectOpt = element.tryCast<SVGRectElement>();
    EXPECT_FALSE(rectOpt.has_value());
    auto groupOpt = element.tryCast<SVGGElement>();
    EXPECT_FALSE(groupOpt.has_value());
  }
}

TEST_F(SVGElementTests, Id) {
  auto element = create();
  EXPECT_EQ(element.id(), "");

  element.setId("test");
  EXPECT_EQ(element.id(), "test");
  EXPECT_THAT(element.getAttribute("id"), testing::Optional(RcString("test")));

  element.setId("");
  EXPECT_EQ(element.id(), "");
  EXPECT_THAT(element.getAttribute("id"), testing::Optional(RcString("")));

  // createWithId is a helper that does the same thing
  EXPECT_EQ(createWithId("asdf").id(), "asdf");

  // Now verify setAttribute can affect the return value of \ref SVGElement::id.
  element.setAttribute("id", "abcd");
  EXPECT_EQ(element.id(), "abcd");
}

TEST_F(SVGElementTests, Type) {
  auto element = create();
  EXPECT_EQ(element.type(), ElementType::Unknown);
  EXPECT_EQ(element.tagName().toString(), "unknown");

  auto rectElement = createRect();
  EXPECT_EQ(rectElement.type(), ElementType::Rect);
  EXPECT_EQ(rectElement.tagName().toString(), "rect");
}

TEST_F(SVGElementTests, ClassName) {
  auto element = create();
  EXPECT_EQ(element.className(), "");

  element.setClassName("test");
  EXPECT_EQ(element.className(), "test");

  EXPECT_THAT(element.getAttribute("class"), testing::Optional(RcString("test")));

  // Now verify setAttribute can affect the return value of \ref SVGElement::className.
  element.setAttribute("class", "abcd");
  EXPECT_EQ(element.className(), "abcd");
}

TEST_F(SVGElementTests, ClassNameMarksStyleCascadeDirty) {
  auto element = create();
  element.setClassName("test");

  const auto* dirty = element.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(dirty, nullptr);
  EXPECT_TRUE(dirty->test(components::DirtyFlagsComponent::StyleCascade));
}

TEST_F(SVGElementTests, Style) {
  auto element = create();
  EXPECT_THAT(element.getAttribute("style"), testing::Eq(std::nullopt));

  element.setStyle("color: red");
  EXPECT_THAT(element.getAttribute("style"), testing::Optional(RcString("color: red")));
}

TEST_F(SVGElementTests, UpdateStyleMarksStyleCascadeDirty) {
  auto element = create();
  element.setStyle("fill: red");
  element.entityHandle().remove<components::DirtyFlagsComponent>();

  element.updateStyle("stroke: blue");

  const auto* dirty = element.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(dirty, nullptr);
  EXPECT_TRUE(dirty->test(components::DirtyFlagsComponent::StyleCascade));
}

TEST_F(SVGElementTests, StyleDirtyPropagatesToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
      <rect id="sibling" width="10" height="10" />
    </svg>
  )");

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  auto sibling = doc.querySelector("#sibling");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(sibling.has_value());

  doc.registry().clear<components::DirtyFlagsComponent>();
  parent->setStyle("fill: red");

  const auto* parentDirty =
      parent->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::StyleCascade));

  const auto* childDirty =
      child->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(childDirty, nullptr);
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::Style));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::Paint));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::RenderInstance));

  EXPECT_EQ(sibling->entityHandle().try_get<components::DirtyFlagsComponent>(), nullptr);
}

TEST_F(SVGElementTests, TransformDirtyPropagatesWorldTransformToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
      <rect id="sibling" width="10" height="10" />
    </svg>
  )");

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  auto sibling = doc.querySelector("#sibling");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(sibling.has_value());

  doc.registry().clear<components::DirtyFlagsComponent>();
  parent->setAttribute("transform", "translate(10 20)");

  const auto* parentDirty =
      parent->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::Transform));
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::WorldTransform));
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::RenderInstance));

  const auto* childDirty =
      child->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(childDirty, nullptr);
  EXPECT_FALSE(childDirty->test(components::DirtyFlagsComponent::Transform));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::WorldTransform));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::RenderInstance));

  EXPECT_EQ(sibling->entityHandle().try_get<components::DirtyFlagsComponent>(), nullptr);
}

TEST_F(SVGElementTests, InheritedPresentationAttributePropagatesToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
      <rect id="sibling" width="10" height="10" />
    </svg>
  )");

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  auto sibling = doc.querySelector("#sibling");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(sibling.has_value());

  doc.registry().clear<components::DirtyFlagsComponent>();
  parent->setAttribute("fill", "red");

  const auto* parentDirty =
      parent->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::StyleCascade));

  const auto* childDirty =
      child->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(childDirty, nullptr);
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::Style));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::Paint));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::RenderInstance));

  EXPECT_EQ(sibling->entityHandle().try_get<components::DirtyFlagsComponent>(), nullptr);
}

TEST_F(SVGElementTests, NonInheritedPresentationAttributeDoesNotPropagateToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent">
        <rect id="child" width="10" height="10" />
      </g>
      <rect id="sibling" width="10" height="10" />
    </svg>
  )");

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  auto sibling = doc.querySelector("#sibling");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(sibling.has_value());

  doc.registry().clear<components::DirtyFlagsComponent>();
  parent->setAttribute("opacity", "0.5");

  const auto* parentDirty =
      parent->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::StyleCascade));

  EXPECT_EQ(child->entityHandle().try_get<components::DirtyFlagsComponent>(), nullptr);
  EXPECT_EQ(sibling->entityHandle().try_get<components::DirtyFlagsComponent>(), nullptr);
}

TEST_F(SVGElementTests, RemovingInheritedPresentationAttributePropagatesToDescendants) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent" fill="blue">
        <rect id="child" width="10" height="10" />
      </g>
      <rect id="sibling" width="10" height="10" />
    </svg>
  )");

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  auto sibling = doc.querySelector("#sibling");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(sibling.has_value());

  doc.registry().clear<components::DirtyFlagsComponent>();
  parent->removeAttribute("fill");

  const auto* parentDirty =
      parent->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::StyleCascade));

  const auto* childDirty =
      child->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(childDirty, nullptr);
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::Style));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::Paint));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::RenderInstance));

  EXPECT_EQ(sibling->entityHandle().try_get<components::DirtyFlagsComponent>(), nullptr);
}

TEST_F(SVGElementTests, RemovingTransformPropagatesWorldTransformToDescendants) {
  auto doc = parseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent" transform="translate(10 20)">
        <rect id="child" width="10" height="10" />
      </g>
      <rect id="sibling" width="10" height="10" />
    </svg>
  )svg");

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  auto sibling = doc.querySelector("#sibling");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(sibling.has_value());

  doc.registry().clear<components::DirtyFlagsComponent>();
  parent->removeAttribute("transform");

  const auto* parentDirty =
      parent->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::Transform));
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::WorldTransform));
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::RenderInstance));

  const auto* childDirty =
      child->entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(childDirty, nullptr);
  EXPECT_FALSE(childDirty->test(components::DirtyFlagsComponent::Transform));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::WorldTransform));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::RenderInstance));

  EXPECT_EQ(sibling->entityHandle().try_get<components::DirtyFlagsComponent>(), nullptr);
}

TEST_F(SVGElementTests, GeometryPresentationAttributeMarksShapeDirty) {
  auto rect = createRect();
  rect.setAttribute("x", "10");

  const auto* dirty = rect.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(dirty, nullptr);
  EXPECT_TRUE(dirty->test(components::DirtyFlagsComponent::Shape));
  EXPECT_TRUE(dirty->test(components::DirtyFlagsComponent::StyleCascade));
}

TEST_F(SVGElementTests, InheritedStyleChangeRecomputesDescendantStyle) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent" fill="blue">
        <rect id="child" width="10" height="10" />
      </g>
    </svg>
  )");

  auto parent = doc.querySelector("#parent");
  auto child = doc.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());

  components::StyleSystem styleSystem;
  styleSystem.computeAllStyles(doc.registry(), nullptr);

  const auto* initialComputed =
      child->entityHandle().try_get<components::ComputedStyleComponent>();
  ASSERT_NE(initialComputed, nullptr);
  ASSERT_TRUE(initialComputed->properties.has_value());
  EXPECT_EQ(initialComputed->properties->fill.getRequired(),
            PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 255))));

  parent->setAttribute("fill", "red");

  EXPECT_EQ(child->entityHandle().try_get<components::ComputedStyleComponent>(), nullptr);

  styleSystem.computeAllStyles(doc.registry(), nullptr);

  const auto* updatedComputed =
      child->entityHandle().try_get<components::ComputedStyleComponent>();
  ASSERT_NE(updatedComputed, nullptr);
  ASSERT_TRUE(updatedComputed->properties.has_value());
  EXPECT_EQ(updatedComputed->properties->fill.getRequired(),
            PaintServer::Solid(css::Color(css::RGBA::RGB(255, 0, 0))));
}

TEST_F(SVGElementTests, SelectiveStyleRecomputeSkipsCleanSiblingsAfterFirstBuild) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" width="10" height="10" fill="red" />
      <rect id="sibling" width="10" height="10" fill="blue" />
    </svg>
  )");

  auto target = doc.querySelector("#target");
  auto sibling = doc.querySelector("#sibling");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(sibling.has_value());

  components::StyleSystem styleSystem;
  styleSystem.computeAllStyles(doc.registry(), nullptr);

  if (!doc.registry().ctx().contains<components::RenderTreeState>()) {
    doc.registry().ctx().emplace<components::RenderTreeState>();
  }
  auto& renderState = doc.registry().ctx().get<components::RenderTreeState>();
  renderState.hasBeenBuilt = true;
  renderState.needsFullRebuild = false;
  renderState.needsFullStyleRecompute = false;

  auto* siblingComputed =
      sibling->entityHandle().try_get<components::ComputedStyleComponent>();
  ASSERT_NE(siblingComputed, nullptr);
  ASSERT_TRUE(siblingComputed->properties.has_value());

  siblingComputed->properties->fill.set(
      PaintServer::Solid(css::Color(css::RGBA::RGB(0, 255, 0))),
      css::Specificity::Override());

  target->entityHandle().get_or_emplace<components::StyleComponent>().properties.opacity.set(
      0.5, css::Specificity::Override());
  components::StyleSystem().invalidateComputed(target->entityHandle());
  target->entityHandle().get_or_emplace<components::DirtyFlagsComponent>().mark(
      components::DirtyFlagsComponent::Style);

  styleSystem.computeAllStyles(doc.registry(), nullptr);

  const auto* targetComputed =
      target->entityHandle().try_get<components::ComputedStyleComponent>();
  ASSERT_NE(targetComputed, nullptr);
  ASSERT_TRUE(targetComputed->properties.has_value());
  EXPECT_DOUBLE_EQ(targetComputed->properties->opacity.getRequired(), 0.5);

  siblingComputed = sibling->entityHandle().try_get<components::ComputedStyleComponent>();
  ASSERT_NE(siblingComputed, nullptr);
  ASSERT_TRUE(siblingComputed->properties.has_value());
  EXPECT_EQ(siblingComputed->properties->fill.getRequired(),
            PaintServer::Solid(css::Color(css::RGBA::RGB(0, 255, 0))));
}

TEST_F(SVGElementTests, NonLocalSelectorMutationRequestsFullStyleRecompute) {
  auto doc = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>
        .trigger + rect { fill: blue; }
      </style>
      <rect id="first" class="trigger" width="10" height="10" />
      <rect id="second" width="10" height="10" />
    </svg>
  )");

  auto first = doc.querySelector("#first");
  auto second = doc.querySelector("#second");
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  components::StyleSystem styleSystem;
  styleSystem.computeAllStyles(doc.registry(), nullptr);

  if (!doc.registry().ctx().contains<components::RenderTreeState>()) {
    doc.registry().ctx().emplace<components::RenderTreeState>();
  }
  auto& renderState = doc.registry().ctx().get<components::RenderTreeState>();
  renderState.hasBeenBuilt = true;
  renderState.needsFullRebuild = false;
  renderState.needsFullStyleRecompute = false;

  const auto* initialComputed =
      second->entityHandle().try_get<components::ComputedStyleComponent>();
  ASSERT_NE(initialComputed, nullptr);
  ASSERT_TRUE(initialComputed->properties.has_value());
  EXPECT_EQ(initialComputed->properties->fill.getRequired(),
            PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 255))));

  first->setClassName("");
  EXPECT_TRUE(renderState.needsFullStyleRecompute);

  styleSystem.computeAllStyles(doc.registry(), nullptr);

  const auto* updatedComputed =
      second->entityHandle().try_get<components::ComputedStyleComponent>();
  ASSERT_NE(updatedComputed, nullptr);
  ASSERT_TRUE(updatedComputed->properties.has_value());
  EXPECT_EQ(updatedComputed->properties->fill.getRequired(),
            PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 0))));
}

TEST_F(SVGElementTests, Attributes) {
  auto element = create();
  EXPECT_THAT(element.getAttribute("foo"), testing::Eq(std::nullopt));
  EXPECT_FALSE(element.hasAttribute("foo"));

  element.setAttribute("foo", "bar");
  EXPECT_THAT(element.getAttribute("foo"), testing::Optional(RcString("bar")));
  EXPECT_TRUE(element.hasAttribute("foo"));

  element.removeAttribute("foo");
  EXPECT_THAT(element.getAttribute("foo"), testing::Eq(std::nullopt));
  EXPECT_FALSE(element.hasAttribute("foo"));
}

TEST_F(SVGElementTests, AppendChildMarksParentAndChildDirty) {
  auto parent = create();
  auto child = create();

  parent.appendChild(child);

  const auto* parentDirty = parent.entityHandle().try_get<components::DirtyFlagsComponent>();
  const auto* childDirty = child.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  ASSERT_NE(childDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::All));
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::All));
}

TEST_F(SVGElementTests, AppendChildMarksInsertedSubtreeDirty) {
  auto parent = create();
  auto child = create();
  auto grandchild = create();
  child.appendChild(grandchild);

  child.entityHandle().remove<components::DirtyFlagsComponent>();
  grandchild.entityHandle().remove<components::DirtyFlagsComponent>();

  parent.appendChild(child);

  const auto* childDirty = child.entityHandle().try_get<components::DirtyFlagsComponent>();
  const auto* grandchildDirty =
      grandchild.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(childDirty, nullptr);
  ASSERT_NE(grandchildDirty, nullptr);
  EXPECT_TRUE(childDirty->test(components::DirtyFlagsComponent::All));
  EXPECT_TRUE(grandchildDirty->test(components::DirtyFlagsComponent::All));
}

TEST_F(SVGElementTests, InsertBeforeMarksParentAndInsertedChildDirty) {
  auto parent = create();
  auto first = create();
  auto inserted = create();
  parent.appendChild(first);
  parent.entityHandle().remove<components::DirtyFlagsComponent>();
  first.entityHandle().remove<components::DirtyFlagsComponent>();

  parent.insertBefore(inserted, first);

  const auto* parentDirty = parent.entityHandle().try_get<components::DirtyFlagsComponent>();
  const auto* insertedDirty = inserted.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  ASSERT_NE(insertedDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::All));
  EXPECT_TRUE(insertedDirty->test(components::DirtyFlagsComponent::All));
  EXPECT_EQ(first.entityHandle().try_get<components::DirtyFlagsComponent>(), nullptr);
}

TEST_F(SVGElementTests, ReplaceChildMarksParentAndReplacementDirty) {
  auto parent = create();
  auto oldChild = create();
  auto newChild = create();
  parent.appendChild(oldChild);
  parent.entityHandle().remove<components::DirtyFlagsComponent>();
  oldChild.entityHandle().remove<components::DirtyFlagsComponent>();

  parent.replaceChild(newChild, oldChild);

  const auto* parentDirty = parent.entityHandle().try_get<components::DirtyFlagsComponent>();
  const auto* newChildDirty = newChild.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  ASSERT_NE(newChildDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::All));
  EXPECT_TRUE(newChildDirty->test(components::DirtyFlagsComponent::All));
}

TEST_F(SVGElementTests, RemoveChildMarksParentDirty) {
  auto parent = create();
  auto child = create();
  parent.appendChild(child);
  parent.entityHandle().remove<components::DirtyFlagsComponent>();
  child.entityHandle().remove<components::DirtyFlagsComponent>();

  parent.removeChild(child);

  const auto* parentDirty = parent.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::All));
}

TEST_F(SVGElementTests, RemoveMarksParentDirty) {
  auto parent = create();
  auto child = create();
  parent.appendChild(child);
  parent.entityHandle().remove<components::DirtyFlagsComponent>();
  child.entityHandle().remove<components::DirtyFlagsComponent>();

  child.remove();

  const auto* parentDirty = parent.entityHandle().try_get<components::DirtyFlagsComponent>();
  ASSERT_NE(parentDirty, nullptr);
  EXPECT_TRUE(parentDirty->test(components::DirtyFlagsComponent::All));
}

TEST_F(SVGElementTests, TrySetPresentationAttribute) {
  // Create a fresh element (SVGUnknownElement by default in create()).
  auto element = create();

  // 1) Test a known/valid presentation attribute that parses successfully.
  {
    auto result = element.trySetPresentationAttribute("fill", "red");
    EXPECT_THAT(result, ParseResultIs(true));

    // Now confirm that the attribute is indeed set.
    EXPECT_THAT(element.getAttribute("fill"), testing::Optional(RcString("red")));
  }

  // 2) Test a known presentation attribute that fails to parse (e.g. invalid color).
  {
    auto result = element.trySetPresentationAttribute("fill", "this-is-not-a-valid-color");
    EXPECT_THAT(result, ParseErrorIs("Invalid paint server"));

    // Because it failed, it should not be stored and the previous 'fill' value remains.
    EXPECT_THAT(element.getAttribute("fill"), testing::Optional(RcString("red")));
  }

  // 3) Test an attribute name that is not recognized as a presentation attribute.
  {
    auto result = element.trySetPresentationAttribute("fancyNonExistentAttr", "1px");
    // Expect no parse error, but the returned bool is false indicating
    // "not a valid presentation attribute for this element."
    EXPECT_THAT(result, ParseResultIs(false));

    // This means it's not stored as a presentation attribute.
    EXPECT_THAT(element.getAttribute("fancyNonExistentAttr"), testing::Eq(std::nullopt));
  }
}

// Basic tests for each function, extensive coverage exists in tree_component_tests.cc
TEST_F(SVGElementTests, TreeOperations) {
  auto root = create();
  auto child1 = create();
  auto child2 = create();
  auto child3 = create();

  root.insertBefore(child1, std::nullopt);
  EXPECT_THAT(children(root), ElementsAre(child1));
  EXPECT_THAT(child1.parentElement(), Optional(root));

  root.insertBefore(child2, child1);
  EXPECT_THAT(children(root), ElementsAre(child2, child1));

  root.appendChild(child3);
  EXPECT_THAT(children(root), ElementsAre(child2, child1, child3));

  auto child4 = create();
  root.replaceChild(child4, child3);
  EXPECT_THAT(children(root), ElementsAre(child2, child1, child4));

  root.removeChild(child1);
  EXPECT_THAT(children(root), ElementsAre(child2, child4));

  child2.remove();
  EXPECT_THAT(children(root), ElementsAre(child4));

  EXPECT_EQ(root.ownerDocument(), document_);
  EXPECT_EQ(child1.ownerDocument(), document_);
  EXPECT_EQ(child2.ownerDocument(), document_);
  EXPECT_EQ(child3.ownerDocument(), document_);
  EXPECT_EQ(child4.ownerDocument(), document_);
}

TEST_F(SVGElementTests, Transform) {
  auto element = create();
  element.setStyle("transform: translate(1px, 2px)");

  EXPECT_THAT(element.transform(), TransformIs(1, 0, 0, 1, 1, 2));
}

TEST_F(SVGElementTests, AbsoluteTransform) {
  auto document = parseSVG(R"-(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <rect id="rect1" x="10" y="10" width="100" height="100" transform="translate(10 20)" />
      <g transform="scale(2)">
        <rect id="rect2" x="10" y="10" width="100" height="100" transform="translate(-10 -20)" />
      </g>
    </svg>
    )-");

  auto rect1 = document.querySelector("#rect1").value().cast<SVGRectElement>();
  EXPECT_THAT(rect1.elementFromWorld(), TransformEq(Transformd::Translate({10, 20})));

  auto rect2 = document.querySelector("#rect2").value().cast<SVGRectElement>();
  EXPECT_THAT(rect2.elementFromWorld(),
              TransformEq(Transformd::Translate({-10, -20}) * Transformd::Scale({2, 2})));
}

TEST_F(SVGElementTests, QuerySelector) {
  {
    auto document = parseSVG(R"(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
        <rect id="rect1" x="10" y="10" width="100" height="100" />
        <rect id="rect2" x="10" y="10" width="100" height="100" />
      </svg>
    )");

    auto element = document.svgElement();

    EXPECT_THAT(element.querySelector("rect"), Optional(ElementIdEq("rect1")));
    EXPECT_THAT(element.querySelector("#rect2"), Optional(ElementIdEq("rect2")));
    EXPECT_THAT(element.querySelector("svg > :nth-child(2)"), Optional(ElementIdEq("rect2")));
    EXPECT_THAT(element.querySelector("does-not-exist"), testing::Eq(std::nullopt));
  }

  // Validate `:scope`
  {
    auto document = parseSVG(R"(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
        <rect id="rect1" x="10" y="10" width="100" height="100" />
        <rect id="rect2" x="10" y="10" width="100" height="100" />
        <g>
          <rect id="rect3" x="10" y="10" width="100" height="100" />
          <rect id="rect4" x="10" y="10" width="100" height="100" />
        </g>
      </svg>
    )");

    auto svgElement = document.svgElement();
    auto gElement = svgElement.querySelector("g");
    ASSERT_THAT(gElement, testing::Ne(std::nullopt));
    EXPECT_EQ(gElement->type(), ElementType::G);

    auto gScopeResult = gElement->querySelector(":scope > rect");
    EXPECT_THAT(gScopeResult, Optional(ElementIdEq("rect3")));
    EXPECT_THAT(gScopeResult->type(), ElementType::Rect);

    auto svgScopeResult = svgElement.querySelector(":scope > rect");
    EXPECT_THAT(svgScopeResult, Optional(ElementIdEq("rect1")));
    EXPECT_THAT(svgScopeResult->type(), ElementType::Rect);
  }
}

TEST_F(SVGElementTests, IsKnownType) {
  auto unknown = create();  // by default "unknown" from create()
  EXPECT_FALSE(unknown.isKnownType());
  EXPECT_EQ(unknown.type(), ElementType::Unknown);

  auto rect = createRect();
  EXPECT_TRUE(rect.isKnownType());
  EXPECT_EQ(rect.type(), ElementType::Rect);
}

TEST_F(SVGElementTests, IsKnownTypeWhenParsed) {
  auto rectDocument = parseSVG(R"(<svg><rect id="myRect" /></svg>)");

  auto maybeRectFromTree = rectDocument.svgElement().firstChild();
  ASSERT_TRUE(maybeRectFromTree.has_value());
  EXPECT_TRUE(maybeRectFromTree->isKnownType());  // <rect> is recognized as known
  EXPECT_EQ(maybeRectFromTree->type(), ElementType::Rect);

  auto maybeRectQuery = rectDocument.querySelector("#myRect");
  ASSERT_TRUE(maybeRectQuery.has_value());
  EXPECT_TRUE(maybeRectQuery->isKnownType());  // <rect> is recognized as known
  EXPECT_EQ(maybeRectQuery->type(), ElementType::Rect);

  EXPECT_EQ(maybeRectFromTree, maybeRectQuery);
}

TEST_F(SVGElementTests, EntityHandle) {
  // Test that entityHandle() returns a valid ECS handle
  auto element = create();
  auto handle = element.entityHandle();
  // Just basic checks: handle should not be null and should be the same when retrieved again
  EXPECT_TRUE(handle.valid());

  auto handle2 = element.entityHandle();
  EXPECT_EQ(handle, handle2);
}

TEST_F(SVGElementTests, UpdateStyle) {
  // This tests setting an initial style, then updating only part of it.
  auto element = create();
  // Start with multiple style attributes
  element.setStyle("fill: red; stroke: blue; opacity: 0.8");

  // updateStyle(...) merges in new or updated properties
  element.updateStyle("stroke: green; visibility: hidden");
  // Expect final style to have fill=red, stroke=green, opacity=0.8, visibility=hidden
  // The exact location of the stored style depends on your implementation. If your code
  // moves them to presentation attributes, you might check getAttribute("stroke") etc.
  // Here, we assume you can see them in getAttribute("style"):
  auto maybeStyle = element.getAttribute("style");
  ASSERT_TRUE(maybeStyle.has_value());

  // The order of properties might differ, so check logically rather than string matching:
  RcString styleString = maybeStyle.value();
  EXPECT_THAT(styleString, testing::HasSubstr("fill: red"));
  EXPECT_THAT(styleString, testing::HasSubstr("stroke: green"));
  EXPECT_THAT(styleString, testing::Not(testing::HasSubstr("stroke: blue")));
  EXPECT_THAT(styleString, testing::HasSubstr("opacity: 0.8"));
  EXPECT_THAT(styleString, testing::HasSubstr("visibility: hidden"));
}

// Verify that the merged style attribute string, when reparsed into a fresh PropertyRegistry,
// produces property values that match what you'd get from applying both existing + update to
// a PropertyRegistry via parseStyle (the additive path).
TEST_F(SVGElementTests, UpdateStyleMergedStringMatchesPropertyRegistry) {
  auto element = create();
  element.setStyle("fill: red; stroke: blue; opacity: 0.8");
  element.updateStyle("stroke: green; visibility: hidden");

  // Get the merged style string from the attribute.
  auto maybeStyle = element.getAttribute("style");
  ASSERT_TRUE(maybeStyle.has_value());

  // Parse the merged style string into a fresh PropertyRegistry.
  PropertyRegistry fromMergedString;
  fromMergedString.parseStyle(maybeStyle.value());

  // Build the expected registry by applying existing then update (additive).
  PropertyRegistry expected;
  expected.parseStyle("fill: red; stroke: blue; opacity: 0.8");
  expected.parseStyle("stroke: green; visibility: hidden");

  // Compare individual properties.
  EXPECT_EQ(fromMergedString.fill.get(), expected.fill.get());
  EXPECT_EQ(fromMergedString.stroke.get(), expected.stroke.get());
  EXPECT_EQ(fromMergedString.opacity.get(), expected.opacity.get());
  EXPECT_EQ(fromMergedString.visibility.get(), expected.visibility.get());
}

TEST_F(SVGElementTests, UpdateStyleMergedStringMatchesRegistryColorOverride) {
  auto element = create();
  element.setStyle("fill: #ff0000; stroke-width: 2px");
  element.updateStyle("fill: rgb(0, 128, 0); stroke-opacity: 0.5");

  auto maybeStyle = element.getAttribute("style");
  ASSERT_TRUE(maybeStyle.has_value());

  PropertyRegistry fromMergedString;
  fromMergedString.parseStyle(maybeStyle.value());

  PropertyRegistry expected;
  expected.parseStyle("fill: #ff0000; stroke-width: 2px");
  expected.parseStyle("fill: rgb(0, 128, 0); stroke-opacity: 0.5");

  EXPECT_EQ(fromMergedString.fill.get(), expected.fill.get());
  EXPECT_EQ(fromMergedString.strokeWidth.get(), expected.strokeWidth.get());
  EXPECT_EQ(fromMergedString.strokeOpacity.get(), expected.strokeOpacity.get());
}

TEST_F(SVGElementTests, UpdateStyleMergedStringMatchesRegistryAllOverridden) {
  auto element = create();
  element.setStyle("fill: red; stroke: blue");
  element.updateStyle("fill: green; stroke: orange");

  auto maybeStyle = element.getAttribute("style");
  ASSERT_TRUE(maybeStyle.has_value());

  PropertyRegistry fromMergedString;
  fromMergedString.parseStyle(maybeStyle.value());

  PropertyRegistry expected;
  expected.parseStyle("fill: red; stroke: blue");
  expected.parseStyle("fill: green; stroke: orange");

  EXPECT_EQ(fromMergedString.fill.get(), expected.fill.get());
  EXPECT_EQ(fromMergedString.stroke.get(), expected.stroke.get());
}

TEST_F(SVGElementTests, UpdateStyleMergedStringMatchesRegistryNoOverlap) {
  auto element = create();
  element.setStyle("fill: red");
  element.updateStyle("stroke: blue");

  auto maybeStyle = element.getAttribute("style");
  ASSERT_TRUE(maybeStyle.has_value());

  PropertyRegistry fromMergedString;
  fromMergedString.parseStyle(maybeStyle.value());

  PropertyRegistry expected;
  expected.parseStyle("fill: red");
  expected.parseStyle("stroke: blue");

  EXPECT_EQ(fromMergedString.fill.get(), expected.fill.get());
  EXPECT_EQ(fromMergedString.stroke.get(), expected.stroke.get());
}

TEST_F(SVGElementTests, UpdateStyleMultipleSequentialUpdates) {
  auto element = create();
  element.setStyle("fill: red; stroke: blue; opacity: 0.5");
  element.updateStyle("stroke: green");
  element.updateStyle("opacity: 1.0; visibility: hidden");

  auto maybeStyle = element.getAttribute("style");
  ASSERT_TRUE(maybeStyle.has_value());

  PropertyRegistry fromMergedString;
  fromMergedString.parseStyle(maybeStyle.value());

  PropertyRegistry expected;
  expected.parseStyle("fill: red; stroke: blue; opacity: 0.5");
  expected.parseStyle("stroke: green");
  expected.parseStyle("opacity: 1.0; visibility: hidden");

  EXPECT_EQ(fromMergedString.fill.get(), expected.fill.get());
  EXPECT_EQ(fromMergedString.stroke.get(), expected.stroke.get());
  EXPECT_EQ(fromMergedString.opacity.get(), expected.opacity.get());
  EXPECT_EQ(fromMergedString.visibility.get(), expected.visibility.get());
}

TEST_F(SVGElementTests, UpdateStyleFromEmptyBase) {
  auto element = create();
  // No setStyle — start with nothing.
  element.updateStyle("fill: red; stroke: blue");

  auto maybeStyle = element.getAttribute("style");
  ASSERT_TRUE(maybeStyle.has_value());

  PropertyRegistry fromMergedString;
  fromMergedString.parseStyle(maybeStyle.value());

  PropertyRegistry expected;
  expected.parseStyle("fill: red; stroke: blue");

  EXPECT_EQ(fromMergedString.fill.get(), expected.fill.get());
  EXPECT_EQ(fromMergedString.stroke.get(), expected.stroke.get());
}

TEST_F(SVGElementTests, FindMatchingAttributes) {
  // create() is an Unknown element, but that’s fine for testing generic XML attributes
  auto element = create();
  element.setAttribute("foo", "valueFoo");
  element.setAttribute({"namespace", "bar"}, "valueBar");
  element.setAttribute({"anotherNS", "bar"}, "valueBar2");
  // So we have:
  //   foo="valueFoo"
  //   namespace:bar="valueBar"
  //   anotherNS:bar="valueBar2"

  // 1) findMatchingAttributes("foo") -> [ "foo" ]
  {
    auto matches = element.findMatchingAttributes("foo");
    EXPECT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].name, "foo");
    EXPECT_TRUE(matches[0].namespacePrefix.empty());
  }

  // 2) findMatchingAttributes({"namespace", "bar"}) -> exactly [ "namespace:bar" ]
  {
    auto matches = element.findMatchingAttributes({"namespace", "bar"});
    EXPECT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].name, "bar");
    EXPECT_EQ(matches[0].namespacePrefix, "namespace");
  }

  // 3) Using a wildcard on the namespace, findMatchingAttributes({ "*", "bar" })
  //    Expect matches from both "namespace:bar" and "anotherNS:bar"
  {
    auto matches = element.findMatchingAttributes({"*", "bar"});
    ASSERT_EQ(matches.size(), 2u);
    // Because the order of attributes might not be guaranteed, verify via a set or multiple checks.
    // For simplicity, just do:
    EXPECT_THAT(matches,
                testing::UnorderedElementsAre(xml::XMLQualifiedNameRef("namespace", "bar"),
                                              xml::XMLQualifiedNameRef("anotherNS", "bar")));
  }
}

TEST_F(SVGElementTests, GetComputedStyleBasic) {
  // This is a minimal test verifying getComputedStyle() after setting a property.
  // For more robust style tests, see your existing style test suite (ElementStyleTests).

  // Let’s parse a rectangle with an inline style and a presentation attribute:
  auto doc = parseSVG(R"(
    <svg>
      <rect id="myRect" style="stroke: green" fill="red" />
    </svg>
  )");

  auto maybeRect = doc.querySelector("#myRect");
  ASSERT_TRUE(maybeRect.has_value());

  const auto& computedStyle = maybeRect->getComputedStyle();

  // Expect transform-origin plus the two properties we set.
  EXPECT_EQ(computedStyle.numPropertiesSet(), 3);
}

TEST_F(SVGElementTests, SetAttributeGetAttributeRoundtripNamespaced) {
  auto element = create();

  // Set a namespaced attribute and verify roundtrip.
  element.setAttribute({"xlink", "href"}, "#target");
  EXPECT_THAT(element.getAttribute({"xlink", "href"}), Optional(RcString("#target")));
  EXPECT_TRUE(element.hasAttribute({"xlink", "href"}));

  // A different namespace with the same local name should not match.
  EXPECT_FALSE(element.hasAttribute({"other", "href"}));
  EXPECT_THAT(element.getAttribute({"other", "href"}), testing::Eq(std::nullopt));

  // Overwrite the same namespaced attribute.
  element.setAttribute({"xlink", "href"}, "#updated");
  EXPECT_THAT(element.getAttribute({"xlink", "href"}), Optional(RcString("#updated")));
}

TEST_F(SVGElementTests, SetAttributePresentationAttributeRoundtrip) {
  auto element = create();

  // setAttribute with a known presentation attribute name stores it and parses it.
  element.setAttribute("fill", "blue");
  EXPECT_THAT(element.getAttribute("fill"), Optional(RcString("blue")));
  EXPECT_TRUE(element.hasAttribute("fill"));

  // Overwrite with a new value.
  element.setAttribute("fill", "green");
  EXPECT_THAT(element.getAttribute("fill"), Optional(RcString("green")));

  // setAttribute with an invalid value for a presentation attribute stores as generic attribute.
  element.setAttribute("fill", "not-a-color");
  EXPECT_THAT(element.getAttribute("fill"), Optional(RcString("not-a-color")));
}

TEST_F(SVGElementTests, RemoveAttributePresentation) {
  auto element = create();
  element.setAttribute("fill", "red");
  EXPECT_TRUE(element.hasAttribute("fill"));

  element.removeAttribute("fill");
  EXPECT_FALSE(element.hasAttribute("fill"));
  EXPECT_THAT(element.getAttribute("fill"), testing::Eq(std::nullopt));
}

TEST_F(SVGElementTests, RemoveAttributeId) {
  auto element = create();
  element.setId("myId");
  EXPECT_EQ(element.id(), "myId");
  EXPECT_TRUE(element.hasAttribute("id"));

  element.removeAttribute("id");
  EXPECT_EQ(element.id(), "");
  // After removing, the attribute storage should reflect removal.
  EXPECT_FALSE(element.hasAttribute("id"));
}

TEST_F(SVGElementTests, RemoveAttributeClass) {
  auto element = create();
  element.setClassName("myClass");
  EXPECT_EQ(element.className(), "myClass");
  EXPECT_TRUE(element.hasAttribute("class"));

  element.removeAttribute("class");
  EXPECT_EQ(element.className(), "");
  EXPECT_FALSE(element.hasAttribute("class"));
}

TEST_F(SVGElementTests, RemoveAttributeStyle) {
  auto element = create();
  element.setStyle("fill: red");
  EXPECT_TRUE(element.hasAttribute("style"));

  element.removeAttribute("style");
  EXPECT_FALSE(element.hasAttribute("style"));
}

TEST_F(SVGElementTests, RemoveAttributeNamespaced) {
  auto element = create();
  element.setAttribute({"xlink", "href"}, "#target");
  EXPECT_TRUE(element.hasAttribute({"xlink", "href"}));

  element.removeAttribute({"xlink", "href"});
  EXPECT_FALSE(element.hasAttribute({"xlink", "href"}));
  EXPECT_THAT(element.getAttribute({"xlink", "href"}), testing::Eq(std::nullopt));
}

TEST_F(SVGElementTests, QuerySelectorComplexSelector) {
  auto document = parseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="g1">
        <rect id="rect1" width="10" height="10" />
      </g>
      <g id="g2">
        <rect id="rect2" width="20" height="20" />
        <rect id="rect3" width="30" height="30" />
      </g>
    </svg>
  )");

  auto svgElement = document.svgElement();

  // svg > g:nth-child(2) > rect should match the first rect in g2
  EXPECT_THAT(svgElement.querySelector("svg > g:nth-child(2) > rect"),
              Optional(ElementIdEq("rect2")));

  // :scope > * should match the first direct child of svgElement
  EXPECT_THAT(svgElement.querySelector(":scope > *"), Optional(ElementIdEq("g1")));

  // :scope > g:nth-child(2) should match g2
  EXPECT_THAT(svgElement.querySelector(":scope > g:nth-child(2)"), Optional(ElementIdEq("g2")));
}

TEST_F(SVGElementTests, QuerySelectorInvalidSelector) {
  auto element = create();

  // Invalid CSS selector should return nullopt without crashing.
  EXPECT_THAT(element.querySelector("[[[invalid"), testing::Eq(std::nullopt));
  EXPECT_THAT(element.querySelector(""), testing::Eq(std::nullopt));
}

TEST_F(SVGElementTests, ReplaceChildVerifiesTreeStructure) {
  auto parent = create();
  auto child1 = createWithId("c1");
  auto child2 = createWithId("c2");
  auto child3 = createWithId("c3");
  auto replacement = createWithId("replacement");

  parent.appendChild(child1);
  parent.appendChild(child2);
  parent.appendChild(child3);
  EXPECT_THAT(children(parent), ElementsAre(child1, child2, child3));

  // Replace the middle child.
  parent.replaceChild(replacement, child2);
  EXPECT_THAT(children(parent), ElementsAre(child1, replacement, child3));

  // The old child should no longer have a parent.
  EXPECT_FALSE(child2.parentElement().has_value());

  // The replacement should have the correct parent.
  EXPECT_THAT(replacement.parentElement(), Optional(parent));
}

TEST_F(SVGElementTests, ReplaceChildWithExistingSibling) {
  // When newChild is already a child of the same parent, it should be moved.
  auto parent = create();
  auto child1 = createWithId("c1");
  auto child2 = createWithId("c2");
  auto child3 = createWithId("c3");

  parent.appendChild(child1);
  parent.appendChild(child2);
  parent.appendChild(child3);

  // Replace child1 with child3 (child3 is already in the tree).
  parent.replaceChild(child3, child1);
  // child3 should have moved to child1's position; child1 is removed.
  EXPECT_THAT(children(parent), ElementsAre(child3, child2));
  EXPECT_FALSE(child1.parentElement().has_value());
}

TEST_F(SVGElementTests, InsertBeforeNulloptAppendsWithExistingChildren) {
  auto parent = create();
  auto child1 = createWithId("c1");
  auto child2 = createWithId("c2");
  auto child3 = createWithId("c3");

  parent.appendChild(child1);
  parent.appendChild(child2);

  // insertBefore with nullopt reference should append at the end.
  parent.insertBefore(child3, std::nullopt);
  EXPECT_THAT(children(parent), ElementsAre(child1, child2, child3));
}

TEST_F(SVGElementTests, RemoveOnRootIsNoOp) {
  // An element without a parent (acting as root) should not crash on remove().
  auto element = create();
  EXPECT_FALSE(element.parentElement().has_value());

  // This should be a no-op, not crash or assert.
  element.remove();

  // Element should still be valid and have no parent.
  EXPECT_FALSE(element.parentElement().has_value());
  EXPECT_TRUE(element.entityHandle().valid());
}

TEST_F(SVGElementTests, AppendChildMovesFromAnotherParent) {
  auto parent1 = create();
  auto parent2 = create();
  auto child = createWithId("child");

  parent1.appendChild(child);
  EXPECT_THAT(children(parent1), ElementsAre(child));
  EXPECT_THAT(child.parentElement(), Optional(parent1));

  // Moving child to parent2 should remove it from parent1.
  parent2.appendChild(child);
  EXPECT_THAT(children(parent1), testing::IsEmpty());
  EXPECT_THAT(children(parent2), ElementsAre(child));
  EXPECT_THAT(child.parentElement(), Optional(parent2));
}

TEST_F(SVGElementTests, InsertBeforeMovesFromAnotherParent) {
  auto parent1 = create();
  auto parent2 = create();
  auto existingChild = createWithId("existing");
  auto movedChild = createWithId("moved");

  parent1.appendChild(movedChild);
  parent2.appendChild(existingChild);

  // insertBefore should remove movedChild from parent1 and insert before existingChild in parent2.
  parent2.insertBefore(movedChild, existingChild);
  EXPECT_THAT(children(parent1), testing::IsEmpty());
  EXPECT_THAT(children(parent2), ElementsAre(movedChild, existingChild));
  EXPECT_THAT(movedChild.parentElement(), Optional(parent2));
}

TEST_F(SVGElementTests, MultipleAppendRemoveCycles) {
  auto parent = create();
  auto child = createWithId("child");

  // Cycle 1: add and remove.
  parent.appendChild(child);
  EXPECT_THAT(children(parent), ElementsAre(child));
  parent.removeChild(child);
  EXPECT_THAT(children(parent), testing::IsEmpty());
  EXPECT_FALSE(child.parentElement().has_value());

  // Cycle 2: re-add.
  parent.appendChild(child);
  EXPECT_THAT(children(parent), ElementsAre(child));
  EXPECT_THAT(child.parentElement(), Optional(parent));

  // Cycle 3: remove via child.remove().
  child.remove();
  EXPECT_THAT(children(parent), testing::IsEmpty());

  // Cycle 4: re-add again.
  parent.appendChild(child);
  EXPECT_THAT(children(parent), ElementsAre(child));
  EXPECT_THAT(child.parentElement(), Optional(parent));
}

TEST_F(SVGElementTests, OwnerDocumentReturnsSameDocument) {
  auto element = create();
  auto ownerDoc = element.ownerDocument();
  EXPECT_EQ(ownerDoc, document_);

  // Child elements also return the same document.
  auto child = create();
  element.appendChild(child);
  EXPECT_EQ(child.ownerDocument(), document_);

  // Elements from a parsed document return their parsed document.
  auto parsedDoc = parseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r"/></svg>)");
  auto rect = parsedDoc.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  EXPECT_EQ(rect->ownerDocument(), parsedDoc);
  // And it should NOT equal our test fixture document.
  EXPECT_NE(rect->ownerDocument(), document_);
}

TEST_F(SVGElementTests, TagNameForDifferentElementTypes) {
  // Verify tagName returns the correct value for various element types created via the API.
  auto rect = SVGRectElement::Create(document_);
  EXPECT_EQ(rect.tagName().name, "rect");
  EXPECT_TRUE(rect.tagName().namespacePrefix.empty());

  auto group = SVGGElement::Create(document_);
  EXPECT_EQ(group.tagName().name, "g");
  EXPECT_TRUE(group.tagName().namespacePrefix.empty());

  // Unknown element created with a custom tag name.
  auto unknown = SVGUnknownElement::Create(document_, "custom-element");
  EXPECT_EQ(unknown.tagName().name, "custom-element");
  EXPECT_TRUE(unknown.tagName().namespacePrefix.empty());

  // Verify tagName via a parsed document.
  auto doc = parseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"><circle id="c"/></svg>)");
  auto circle = doc.querySelector("#c");
  ASSERT_TRUE(circle.has_value());
  EXPECT_EQ(circle->tagName().name, "circle");
}

TEST_F(SVGElementTests, IsKnownTypeForVariousElements) {
  // SVGGElement is known.
  auto group = SVGGElement::Create(document_);
  EXPECT_TRUE(group.isKnownType());
  EXPECT_EQ(group.type(), ElementType::G);

  // SVGRectElement is known.
  auto rect = SVGRectElement::Create(document_);
  EXPECT_TRUE(rect.isKnownType());
  EXPECT_EQ(rect.type(), ElementType::Rect);

  // SVGUnknownElement is NOT known.
  auto unknown = SVGUnknownElement::Create(document_, "foobar");
  EXPECT_FALSE(unknown.isKnownType());
  EXPECT_EQ(unknown.type(), ElementType::Unknown);
}

}  // namespace donner::svg
