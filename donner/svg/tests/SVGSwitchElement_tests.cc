#include "donner/svg/SVGSwitchElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Ne;

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

}  // namespace donner::svg
