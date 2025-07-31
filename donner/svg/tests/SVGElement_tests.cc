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
#include "donner/svg/parser/SVGParser.h"

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

TEST_F(SVGElementTests, Style) {
  auto element = create();
  EXPECT_THAT(element.getAttribute("style"), testing::Eq(std::nullopt));

  element.setStyle("color: red");
  EXPECT_THAT(element.getAttribute("style"), testing::Optional(RcString("color: red")));
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

#if 0
// TODO: This needs support for updating the style attribute
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
  EXPECT_THAT(styleString, testing::HasSubstr("opacity: 0.8"));
  EXPECT_THAT(styleString, testing::HasSubstr("visibility: hidden"));
}
#endif

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

}  // namespace donner::svg
