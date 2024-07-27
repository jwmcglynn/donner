#include "donner/svg/SVGElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <deque>

#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGUnknownElement.h"
#include "donner/svg/components/DocumentContext.h"
#include "donner/svg/xml/XMLParser.h"

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
    parser::XMLParser::InputBuffer inputBuffer(input);
    auto maybeResult = parser::XMLParser::ParseSVG(inputBuffer);
    EXPECT_THAT(maybeResult, base::parser::NoParseError());
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

// TODO: trySetPresentationAttribute test

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

// Basic tests for each function, extensive coverage exists in tree_component_tests.cc
TEST_F(SVGElementTests, TreeOperations) {
  auto root = create();
  auto child1 = create();
  auto child2 = create();
  auto child3 = create();

  EXPECT_EQ(child1, root.insertBefore(child1, std::nullopt));
  EXPECT_THAT(children(root), ElementsAre(child1));
  EXPECT_THAT(child1.parentElement(), Optional(root));

  EXPECT_EQ(child2, root.insertBefore(child2, child1));
  EXPECT_THAT(children(root), ElementsAre(child2, child1));

  EXPECT_EQ(child3, root.appendChild(child3));
  EXPECT_THAT(children(root), ElementsAre(child2, child1, child3));

  auto child4 = create();
  EXPECT_EQ(child4, root.replaceChild(child4, child3));
  EXPECT_THAT(children(root), ElementsAre(child2, child1, child4));

  EXPECT_EQ(child1, root.removeChild(child1));
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

    auto gScopeResult = gElement->querySelector(":scope > rect");
    EXPECT_THAT(gScopeResult, Optional(ElementIdEq("rect3")));

    auto svgScopeResult = svgElement.querySelector(":scope > rect");
    EXPECT_THAT(svgScopeResult, Optional(ElementIdEq("rect1")));
  }
}

}  // namespace donner::svg
