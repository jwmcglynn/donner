#include "donner/svg/SVGDescElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGRectElement.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Eq;
using testing::Ne;

namespace donner::svg {

/// @test A `<desc>` element parses to the correct type and is not an unknown element.
TEST(SVGDescElementTests, RecognizedElementType) {
  auto desc = instantiateSubtreeElementAs<SVGDescElement>("<desc>Details</desc>");

  EXPECT_THAT(desc->type(), Eq(ElementType::Desc));
  EXPECT_THAT(desc->tryCast<SVGDescElement>(), Ne(std::nullopt));
}

/// @test The text content of a `<desc>` is exposed through the DOM.
TEST(SVGDescElementTests, TextContentAccessible) {
  auto desc = instantiateSubtreeElementAs<SVGDescElement>(
      "<desc>A solid blue circle indicating the service is online.</desc>");

  EXPECT_THAT(desc->textContent(),
              Eq("A solid blue circle indicating the service is online."));
}

/// @test `setTextContent` updates the text exposed by `textContent`.
TEST(SVGDescElementTests, SetTextContent) {
  SVGDocument document;
  SVGDescElement desc = SVGDescElement::Create(document);

  desc.setTextContent("Updated description");
  EXPECT_THAT(desc.textContent(), Eq("Updated description"));
}

/// @test A `<desc>` nested inside a shape parses and exposes its text.
TEST(SVGDescElementTests, NestedDescInShape) {
  auto rect = instantiateSubtreeElementAs<SVGRectElement>(
      R"(<rect x="1" y="1" width="4" height="4"><desc>Rectangle description</desc></rect>)");

  std::optional<SVGElement> child = rect->firstChild();
  ASSERT_THAT(child, Ne(std::nullopt));

  std::optional<SVGDescElement> desc = child->tryCast<SVGDescElement>();
  ASSERT_THAT(desc, Ne(std::nullopt));
  EXPECT_THAT(desc->textContent(), Eq("Rectangle description"));
}

/// @test A `<desc>` (even one containing graphics) inside a rect-bearing group changes no pixels.
TEST(SVGDescElementTests, NotRenderedChangesNoPixels) {
  const AsciiImage withDesc = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <g>
        <rect x="4" y="4" width="8" height="8" fill="white"/>
        <desc>A white square<circle cx="8" cy="8" r="6" fill="white"/></desc>
      </g>
    </svg>
  )");

  const AsciiImage withoutDesc = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <g>
        <rect x="4" y="4" width="8" height="8" fill="white"/>
      </g>
    </svg>
  )");

  EXPECT_EQ(withDesc.generated, withoutDesc.generated);
}

}  // namespace donner::svg
