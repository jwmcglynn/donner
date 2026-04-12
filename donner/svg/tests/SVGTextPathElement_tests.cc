#include "donner/svg/SVGTextPathElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Eq;
using testing::Ne;
using testing::Optional;

namespace donner::svg {
namespace {

constexpr parser::SVGParser::Options kExperimentalOptions = []() {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  return options;
}();

/// Helper to instantiate a textPath inside a text element with a referenced path.
SVGDocument makeTextPathDocument(std::string_view textPathAttrs = R"(href="#myPath")",
                                 std::string_view textContent = "Text on path") {
  const std::string svg = std::string(R"(
    <svg>
      <defs><path id="myPath" d="M 10 80 Q 95 10 180 80" /></defs>
      <text font-family="fallback-font" font-size="16px">
        <textPath )") +
                          std::string(textPathAttrs) + ">" + std::string(textContent) +
                          R"(</textPath>
      </text>
    </svg>
  )";
  return instantiateSubtree(svg, kExperimentalOptions);
}

SVGTextPathElement queryTextPath(SVGDocument& doc) {
  return doc.querySelector("textPath")->cast<SVGTextPathElement>();
}

// ── Type casting tests ─────────────────────────────────────────────────────

TEST(SVGTextPathElementTests, CreateAndCast) {
  auto doc = makeTextPathDocument();
  auto textPath = queryTextPath(doc);

  EXPECT_THAT(textPath.tryCast<SVGTextPathElement>(), Ne(std::nullopt));
  EXPECT_THAT(textPath.tryCast<SVGTextPositioningElement>(), Ne(std::nullopt));
  EXPECT_THAT(textPath.tryCast<SVGTextContentElement>(), Ne(std::nullopt));
  EXPECT_THAT(textPath.tryCast<SVGElement>(), Ne(std::nullopt));
}

// ── Default values ─────────────────────────────────────────────────────────

TEST(SVGTextPathElementTests, DefaultValues) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <defs><path id="myPath" d="M 10 80 Q 95 10 180 80" /></defs>
      <text font-family="fallback-font" font-size="16px">
        <textPath>Text on path</textPath>
      </text>
    </svg>
  )",
                                kExperimentalOptions);
  auto textPath = queryTextPath(doc);

  EXPECT_THAT(textPath.href(), Eq(std::nullopt));
  EXPECT_THAT(textPath.startOffset(), Eq(std::nullopt));
}

// ── Parsing from XML ───────────────────────────────────────────────────────

TEST(SVGTextPathElementTests, ParseHref) {
  auto doc = makeTextPathDocument(R"(href="#myPath")");
  auto textPath = queryTextPath(doc);

  EXPECT_THAT(textPath.href(), Optional(ToStringIs("#myPath")));
}

TEST(SVGTextPathElementTests, ParseStartOffset) {
  auto doc = makeTextPathDocument(R"(href="#myPath" startOffset="50")");
  auto textPath = queryTextPath(doc);

  EXPECT_THAT(textPath.startOffset(), Optional(LengthIs(50.0, Lengthd::Unit::None)));
}

TEST(SVGTextPathElementTests, ParseStartOffsetPx) {
  auto doc = makeTextPathDocument(R"(href="#myPath" startOffset="25px")");
  auto textPath = queryTextPath(doc);

  EXPECT_THAT(textPath.startOffset(), Optional(LengthIs(25.0, Lengthd::Unit::Px)));
}

TEST(SVGTextPathElementTests, ParseStartOffsetPercent) {
  auto doc = makeTextPathDocument(R"(href="#myPath" startOffset="50%")");
  auto textPath = queryTextPath(doc);

  EXPECT_THAT(textPath.startOffset(), Optional(LengthIs(50.0, Lengthd::Unit::Percent)));
}

// ── Setting values programmatically ────────────────────────────────────────

TEST(SVGTextPathElementTests, SetHref) {
  auto doc = makeTextPathDocument();
  auto textPath = queryTextPath(doc);

  textPath.setHref("#otherPath");
  EXPECT_THAT(textPath.href(), Optional(ToStringIs("#otherPath")));
}

TEST(SVGTextPathElementTests, SetStartOffset) {
  auto doc = makeTextPathDocument();
  auto textPath = queryTextPath(doc);

  textPath.setStartOffset(Lengthd(100.0, Lengthd::Unit::Px));
  EXPECT_THAT(textPath.startOffset(), Optional(LengthIs(100.0, Lengthd::Unit::Px)));
}

TEST(SVGTextPathElementTests, SetStartOffsetPercent) {
  auto doc = makeTextPathDocument();
  auto textPath = queryTextPath(doc);

  textPath.setStartOffset(Lengthd(75.0, Lengthd::Unit::Percent));
  EXPECT_THAT(textPath.startOffset(), Optional(LengthIs(75.0, Lengthd::Unit::Percent)));
}

TEST(SVGTextPathElementTests, SetStartOffsetNullopt) {
  auto doc = makeTextPathDocument(R"(href="#myPath" startOffset="50")");
  auto textPath = queryTextPath(doc);

  EXPECT_THAT(textPath.startOffset(), Ne(std::nullopt));
  textPath.setStartOffset(std::nullopt);
  EXPECT_THAT(textPath.startOffset(), Eq(std::nullopt));
}

TEST(SVGTextPathElementTests, AccessorsReturnNulloptWithoutComponent) {
  SVGDocument document;
  SVGTextPathElement textPath = SVGTextPathElement::Create(document);
  textPath.entityHandle().remove<components::TextPathComponent>();

  EXPECT_EQ(textPath.href(), std::nullopt);
  EXPECT_EQ(textPath.startOffset(), std::nullopt);
}

// ── xlink:href support (legacy namespace) ──────────────────────────────────

TEST(SVGTextPathElementTests, XlinkHref) {
  auto doc = instantiateSubtree(R"(
    <svg xmlns:xlink="http://www.w3.org/1999/xlink">
      <defs><path id="myPath" d="M 10 80 Q 95 10 180 80" /></defs>
      <text font-family="fallback-font" font-size="16px">
        <textPath xlink:href="#myPath">Text on path</textPath>
      </text>
    </svg>
  )",
                                kExperimentalOptions);
  auto textPath = queryTextPath(doc);

  EXPECT_THAT(textPath.href(), Optional(ToStringIs("#myPath")));
}

TEST(SVGTextPathElementTests, ParseMethodSideSpacingAndPositioningLists) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <defs><path id="myPath" d="M 10 80 Q 95 10 180 80" /></defs>
      <text font-family="fallback-font" font-size="16px">
        <textPath href="#myPath" startOffset="25%" method="stretch" side="right" spacing="auto"
                  x="1 2%" y="3" dx="4 5" dy="6 7" rotate="0 90deg">
          Text on path
        </textPath>
      </text>
    </svg>
  )",
                            kExperimentalOptions);
  auto textPath = queryTextPath(doc);

  EXPECT_THAT(textPath.startOffset(), Optional(LengthIs(25.0, Lengthd::Unit::Percent)));
  EXPECT_THAT(textPath.xList(),
              testing::ElementsAre(Lengthd(1, Lengthd::Unit::None),
                                   Lengthd(2, Lengthd::Unit::Percent)));
  EXPECT_THAT(textPath.yList(), testing::ElementsAre(Lengthd(3, Lengthd::Unit::None)));
  EXPECT_THAT(textPath.dxList(),
              testing::ElementsAre(Lengthd(4, Lengthd::Unit::None),
                                   Lengthd(5, Lengthd::Unit::None)));
  EXPECT_THAT(textPath.dyList(),
              testing::ElementsAre(Lengthd(6, Lengthd::Unit::None),
                                   Lengthd(7, Lengthd::Unit::None)));
  EXPECT_THAT(textPath.rotateList(), testing::ElementsAre(0.0, 90.0));

  const auto& component = textPath.entityHandle().get<components::TextPathComponent>();
  EXPECT_EQ(component.method, components::TextPathMethod::Stretch);
  EXPECT_EQ(component.side, components::TextPathSide::Right);
  EXPECT_EQ(component.spacing, components::TextPathSpacing::Auto);
}

}  // namespace
}  // namespace donner::svg
