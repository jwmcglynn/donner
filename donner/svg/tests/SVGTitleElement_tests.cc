#include "donner/svg/SVGTitleElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGRectElement.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Eq;
using testing::Ne;

namespace donner::svg {

/// @test A `<title>` element parses to the correct type and is not an unknown element.
TEST(SVGTitleElementTests, RecognizedElementType) {
  auto title = instantiateSubtreeElementAs<SVGTitleElement>("<title>Hello</title>");

  EXPECT_THAT(title->type(), Eq(ElementType::Title));
  EXPECT_THAT(title->tryCast<SVGTitleElement>(), Ne(std::nullopt));
}

/// @test The text content of a `<title>` is exposed through the DOM.
TEST(SVGTitleElementTests, TextContentAccessible) {
  auto title = instantiateSubtreeElementAs<SVGTitleElement>("<title>A blue circle</title>");

  EXPECT_THAT(title->textContent(), Eq("A blue circle"));
}

/// @test An empty `<title>` reports empty text content.
TEST(SVGTitleElementTests, EmptyTextContent) {
  auto title = instantiateSubtreeElementAs<SVGTitleElement>("<title></title>");

  EXPECT_THAT(title->textContent(), Eq(""));
}

/// @test `setTextContent` updates the text exposed by `textContent`.
TEST(SVGTitleElementTests, SetTextContent) {
  SVGDocument document;
  SVGTitleElement title = SVGTitleElement::Create(document);

  title.setTextContent("Updated title");
  EXPECT_THAT(title.textContent(), Eq("Updated title"));
}

/// @test A `<title>` nested inside a shape parses and exposes its text.
TEST(SVGTitleElementTests, NestedTitleInShape) {
  auto rect = instantiateSubtreeElementAs<SVGRectElement>(
      R"(<rect x="1" y="1" width="4" height="4"><title>Rectangle label</title></rect>)");

  std::optional<SVGElement> child = rect->firstChild();
  ASSERT_THAT(child, Ne(std::nullopt));

  std::optional<SVGTitleElement> title = child->tryCast<SVGTitleElement>();
  ASSERT_THAT(title, Ne(std::nullopt));
  EXPECT_THAT(title->textContent(), Eq("Rectangle label"));
}

/// @test A `<title>` (even one containing graphics) inside a rect-bearing group changes no pixels.
TEST(SVGTitleElementTests, NotRenderedChangesNoPixels) {
  const AsciiImage withTitle = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <g>
        <rect x="4" y="4" width="8" height="8" fill="white"/>
        <title>A white square<circle cx="8" cy="8" r="6" fill="white"/></title>
      </g>
    </svg>
  )");

  const AsciiImage withoutTitle = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <g>
        <rect x="4" y="4" width="8" height="8" fill="white"/>
      </g>
    </svg>
  )");

  EXPECT_EQ(withTitle.generated, withoutTitle.generated);
}

}  // namespace donner::svg
