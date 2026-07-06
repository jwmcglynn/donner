#include "donner/svg/SVGMetadataElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Eq;
using testing::Ne;

namespace donner::svg {

/// @test A `<metadata>` element parses to the correct type and is not an unknown element.
TEST(SVGMetadataElementTests, RecognizedElementType) {
  auto metadata = instantiateSubtreeElementAs<SVGMetadataElement>("<metadata>info</metadata>");

  EXPECT_THAT(metadata->type(), Eq(ElementType::Metadata));
  EXPECT_THAT(metadata->tryCast<SVGMetadataElement>(), Ne(std::nullopt));
}

/// @test The concatenated text content of a `<metadata>` is exposed through the DOM.
TEST(SVGMetadataElementTests, TextContentAccessible) {
  auto metadata =
      instantiateSubtreeElementAs<SVGMetadataElement>("<metadata>Author: Jane Doe</metadata>");

  EXPECT_THAT(metadata->textContent(), Eq("Author: Jane Doe"));
}

/// @test `setTextContent` updates the text exposed by `textContent`.
TEST(SVGMetadataElementTests, SetTextContent) {
  SVGDocument document;
  SVGMetadataElement metadata = SVGMetadataElement::Create(document);

  metadata.setTextContent("Updated metadata");
  EXPECT_THAT(metadata.textContent(), Eq("Updated metadata"));
}

/// @test A `<metadata>` element with content changes no pixels.
TEST(SVGMetadataElementTests, NotRenderedChangesNoPixels) {
  const AsciiImage withMetadata = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <g>
        <rect x="4" y="4" width="8" height="8" fill="white"/>
        <metadata><circle cx="8" cy="8" r="6" fill="white"/></metadata>
      </g>
    </svg>
  )");

  const AsciiImage withoutMetadata = RendererTestUtils::renderToAsciiImage(R"(
    <svg width="16" height="16">
      <g>
        <rect x="4" y="4" width="8" height="8" fill="white"/>
      </g>
    </svg>
  )");

  EXPECT_EQ(withMetadata.generated, withoutMetadata.generated);
}

}  // namespace donner::svg
