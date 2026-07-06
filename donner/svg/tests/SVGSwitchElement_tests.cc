#include "donner/svg/SVGSwitchElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/RcString.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Ne;
using testing::Optional;

namespace donner::svg {

namespace {

/// 16x16 ASCII golden: left half filled (black => `@`), right half empty.
constexpr std::string_view kLeftHalfFilled = R"(
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
    @@@@@@@@........
  )";

/// 16x16 ASCII golden: fully filled (black => `@`).
constexpr std::string_view kAllFilled = R"(
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
    @@@@@@@@@@@@@@@@
  )";

/// 16x16 ASCII golden: fully empty.
constexpr std::string_view kAllEmpty = R"(
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    ................
  )";

}  // namespace

/**
 * Test that a `<switch>` element can be created and safely cast to its base type.
 */
TEST(SVGSwitchElementTests, CreateAndCast) {
  auto switchElement = instantiateSubtreeElementAs<SVGSwitchElement>("<switch></switch>");

  // Verify that the switch element can be cast to SVGGraphicsElement (its base class)
  EXPECT_THAT(switchElement->tryCast<SVGGraphicsElement>(), Ne(std::nullopt));
  // And that casting to itself works.
  EXPECT_THAT(switchElement->tryCast<SVGSwitchElement>(), Ne(std::nullopt));
}

/**
 * The first child whose conditional attributes all pass renders; later children do not render,
 * even though they would also pass.
 */
TEST(SVGSwitchElementTests, RendersOnlyFirstMatchingChild) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <switch>
        <rect x="0" y="0" width="8" height="16" fill="black"/>
        <rect x="0" y="0" width="16" height="16" fill="black"/>
      </switch>
    </svg>
  )");

  EXPECT_TRUE(generatedAscii.matches(kLeftHalfFilled));
}

/**
 * A child with a non-empty `requiredExtensions` fails its conditional and is skipped; the next
 * child is selected.
 */
TEST(SVGSwitchElementTests, SkipsChildWithFailingRequiredExtensions) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <switch>
        <rect x="0" y="0" width="16" height="16" fill="black"
              requiredExtensions="http://example.org/bogus"/>
        <rect x="0" y="0" width="8" height="16" fill="black"/>
      </switch>
    </svg>
  )");

  EXPECT_TRUE(generatedAscii.matches(kLeftHalfFilled));
}

/**
 * A `<switch>` inserted through the source-edit projection path gets the same element type as a
 * full SVG parser load, so rendering uses switch child selection instead of generic traversal.
 */
TEST(SVGSwitchElementTests, SourceEditInsertedSwitchProjectsAsSwitch) {
  const std::string input = R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <g id="layer"><rect id="existing" width="0" height="0"/></g>
    </svg>
  )";
  parser::SVGParser::Options options;
  options.parseAsInlineSVG = true;
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  auto maybeDocument = parser::SVGParser::ParseSVG(input, disabledSink, options);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();
  SVGDocument document = std::move(maybeDocument).result();
  const std::string switchSource =
      R"(<switch id="inserted">)"
      R"(<rect x="0" y="0" width="16" height="16" fill="black" systemLanguage="ru-RU"/>)"
      R"(<rect x="0" y="0" width="8" height="16" fill="black"/>)"
      R"(</switch>)";
  const std::size_t insertionOffset = input.find("</g>");
  ASSERT_NE(insertionOffset, std::string_view::npos);

  xml::ApplySourceEditResult result = document.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(insertionOffset), FileOffset::Offset(insertionOffset)},
      .replacement = switchSource,
      .sourceVersion = document.sourceVersion(),
  });

  ASSERT_FALSE(result.diagnostic.has_value()) << *result.diagnostic;
  EXPECT_TRUE(result.applied);
  ASSERT_FALSE(result.mutations.empty());

  std::optional<SVGElement> layer = document.svgElement().firstChild();
  ASSERT_TRUE(layer.has_value());
  EXPECT_THAT(layer->getAttribute("id"), Optional(RcString("layer")));

  std::optional<SVGElement> inserted = layer->lastChild();
  ASSERT_TRUE(inserted.has_value())
      << "source: " << document.source() << "\nmutations: " << result.mutations.size();
  EXPECT_THAT(inserted->getAttribute("id"), Optional(RcString("inserted")));
  EXPECT_EQ(inserted->type(), ElementType::Switch);
  EXPECT_THAT(inserted->tryCast<SVGSwitchElement>(), Ne(std::nullopt));
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(kLeftHalfFilled));
}

/**
 * `requiredFeatures` always evaluates to true (deprecated in SVG2), so a child declaring it is
 * still selected.
 */
TEST(SVGSwitchElementTests, RequiredFeaturesIsIgnored) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <switch>
        <rect x="0" y="0" width="16" height="16" fill="black"
              requiredFeatures="http://www.w3.org/TR/SVG11/feature#Structure"/>
        <rect x="0" y="0" width="8" height="16" fill="black"/>
      </switch>
    </svg>
  )");

  EXPECT_TRUE(generatedAscii.matches(kAllFilled));
}

/**
 * A child whose `systemLanguage` does not match the user language is skipped; a matching
 * `systemLanguage` (including a more specific subtag such as "en-GB") is selected.
 */
TEST(SVGSwitchElementTests, SystemLanguageSelectsMatchingChild) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <switch>
        <rect x="0" y="0" width="16" height="16" fill="black" systemLanguage="ru-RU"/>
        <rect x="0" y="0" width="8" height="16" fill="black" systemLanguage="en-GB"/>
        <rect x="0" y="0" width="16" height="16" fill="black"/>
      </switch>
    </svg>
  )");

  EXPECT_TRUE(generatedAscii.matches(kLeftHalfFilled));
}

/**
 * Changing a conditional-processing attribute through the DOM mutation path updates the parsed
 * conditional state used by `<switch>` selection.
 */
TEST(SVGSwitchElementTests, SetAttributeUpdatesConditionalSelection) {
  SVGDocument document = instantiateSubtree(R"(
      <switch>
        <rect id="candidate" x="0" y="0" width="16" height="16" fill="black"
              systemLanguage="ru-RU"/>
        <rect x="0" y="0" width="8" height="16" fill="black"/>
      </switch>
    )",
                                            parser::SVGParser::Options(), Vector2i(16, 16));

  std::optional<SVGElement> candidate = document.querySelector("#candidate");
  ASSERT_TRUE(candidate.has_value());
  candidate->setAttribute("systemLanguage", "en");

  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(kAllFilled));
}

/**
 * Removing a conditional-processing attribute through the DOM mutation path clears the parsed
 * conditional state instead of leaving the old value active.
 */
TEST(SVGSwitchElementTests, RemoveAttributeUpdatesConditionalSelection) {
  SVGDocument document = instantiateSubtree(R"(
      <switch>
        <rect id="candidate" x="0" y="0" width="16" height="16" fill="black"
              requiredExtensions="http://example.org/bogus"/>
        <rect x="0" y="0" width="8" height="16" fill="black"/>
      </switch>
    )",
                                            parser::SVGParser::Options(), Vector2i(16, 16));

  std::optional<SVGElement> candidate = document.querySelector("#candidate");
  ASSERT_TRUE(candidate.has_value());
  candidate->removeAttribute("requiredExtensions");

  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(kAllFilled));
}

/**
 * Unknown (non-SVG) child elements are never selected; selection continues with the next child.
 */
TEST(SVGSwitchElementTests, SkipsUnknownElementChild) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <switch>
        <random/>
        <rect x="0" y="0" width="8" height="16" fill="black"/>
      </switch>
    </svg>
  )");

  EXPECT_TRUE(generatedAscii.matches(kLeftHalfFilled));
}

/**
 * `display` does not participate in child selection: a child with `display: none` still wins the
 * selection (its conditionals pass) and then renders nothing, so no fallback appears.
 */
TEST(SVGSwitchElementTests, DisplayNoneChildStillSelected) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <switch>
        <rect x="0" y="0" width="16" height="16" fill="black" display="none"/>
        <rect x="0" y="0" width="16" height="16" fill="black"/>
      </switch>
    </svg>
  )");

  EXPECT_TRUE(generatedAscii.matches(kAllEmpty));
}

/**
 * `<switch>` otherwise behaves like `<g>`: attributes such as `transform` apply to the rendered
 * child.
 */
TEST(SVGSwitchElementTests, AppliesGroupAttributesToSelectedChild) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"SVG(
    <svg width="16" height="16">
      <switch transform="translate(8 0)">
        <rect x="0" y="0" width="8" height="16" fill="black"/>
      </switch>
    </svg>
  )SVG");

  EXPECT_TRUE(generatedAscii.matches(R"(
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
    ........@@@@@@@@
  )"));
}

/**
 * Conditional-processing attributes also disable rendering of elements outside a `<switch>`.
 */
TEST(SVGSwitchElementTests, ConditionalAttributesOutsideSwitchDisableRendering) {
  const AsciiImage nonMatching = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <rect x="0" y="0" width="16" height="16" fill="black" systemLanguage="ru-RU"/>
    </svg>
  )");
  EXPECT_TRUE(nonMatching.matches(kAllEmpty));

  const AsciiImage matching = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <rect x="0" y="0" width="16" height="16" fill="black" systemLanguage="en-GB"/>
    </svg>
  )");
  EXPECT_TRUE(matching.matches(kAllFilled));
}

/**
 * A failing conditional on a container disables its entire subtree.
 */
TEST(SVGSwitchElementTests, FailingConditionalOnContainerDisablesSubtree) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <g requiredExtensions="http://example.org/bogus">
        <rect x="0" y="0" width="16" height="16" fill="black"/>
      </g>
    </svg>
  )");

  EXPECT_TRUE(generatedAscii.matches(kAllEmpty));
}

/**
 * A child that is not a directly-rendered element type (here `<defs>`) is not eligible for
 * selection and does not consume the selection slot, so the following `<rect>` is selected. This
 * distinguishes the whitelist behavior from skipping only unknown element types: `<defs>` is a
 * known element type but is still not rendered by a `<switch>`.
 */
TEST(SVGSwitchElementTests, SwitchSkipsNonRenderedChildTypes) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <switch>
        <defs>
          <rect x="0" y="0" width="16" height="16" fill="black"/>
        </defs>
        <rect x="0" y="0" width="8" height="16" fill="black"/>
      </switch>
    </svg>
  )");

  EXPECT_TRUE(generatedAscii.matches(kLeftHalfFilled));
}

/**
 * When no direct child passes conditional processing, the `<switch>` renders nothing.
 */
TEST(SVGSwitchElementTests, SwitchNoValidChildRendersNothing) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <switch>
        <rect x="0" y="0" width="16" height="16" fill="black"
              requiredExtensions="http://example.org/bogus"/>
        <rect x="0" y="0" width="16" height="16" fill="black"
              systemLanguage="zz"/>
      </switch>
    </svg>
  )");

  EXPECT_TRUE(generatedAscii.matches(kAllEmpty));
}

/**
 * A `<switch>` nested as the first child of another `<switch>` is itself a directly-rendered
 * element type, so the outer switch selects it and the inner switch selects its own first valid
 * child. The outer switch's later children are never rendered.
 */
TEST(SVGSwitchElementTests, NestedSwitch) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <switch>
        <switch>
          <rect x="0" y="0" width="16" height="16" fill="black"
                requiredExtensions="http://example.org/bogus"/>
          <rect x="0" y="0" width="16" height="16" fill="black"/>
        </switch>
        <rect x="0" y="0" width="8" height="16" fill="black"/>
      </switch>
    </svg>
  )");

  // The inner switch selects its full-size second child; the outer switch's 8-wide child is never
  // rendered.
  EXPECT_TRUE(generatedAscii.matches(kAllFilled));
}

/**
 * `systemLanguage` is evaluated against the document's configured user language list. The default
 * `{"en"}` does not match `systemLanguage="ru"`, but configuring the list to `{"ru"}` does.
 */
TEST(SVGSwitchElementTests, SystemLanguageHonorsConfiguredLanguages) {
  constexpr std::string_view kSvg = R"(
    <svg width="16" height="16">
      <rect x="0" y="0" width="16" height="16" fill="black" systemLanguage="ru"/>
    </svg>
  )";

  // Default user language "en" does not match systemLanguage="ru".
  {
    SVGDocument document = instantiateSubtree(kSvg);
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(std::move(document));
    EXPECT_TRUE(generatedAscii.matches(kAllEmpty));
  }

  // After configuring the user language list to {"ru"}, the rect renders.
  {
    SVGDocument document = instantiateSubtree(kSvg);
    document.setUserLanguages({RcString("ru")});
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(std::move(document));
    EXPECT_TRUE(generatedAscii.matches(kAllFilled));
  }
}

}  // namespace donner::svg
