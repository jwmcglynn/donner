#include "donner/svg/SVGClipPathElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/XMLTestUtils.h"

using testing::AllOf;

namespace donner::svg {

TEST(SVGClipPathElementTests, Defaults) {
  auto clipPath = instantiateSubtreeElementAs<SVGClipPathElement>("<clipPath />");
  EXPECT_EQ(clipPath->clipPathUnits(), ClipPathUnits::UserSpaceOnUse);
}

TEST(SVGClipPathElementTests, SetClipPathUnits) {
  auto clipPath = instantiateSubtreeElementAs<SVGClipPathElement>(
      "<clipPath clipPathUnits=\"objectBoundingBox\" />");
  EXPECT_EQ(clipPath->clipPathUnits(), ClipPathUnits::ObjectBoundingBox);
}

TEST(SVGClipPathElementTests, RenderingDefaults) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <clipPath id="a">
          <circle cx="8" cy="8" r="8" />
        </clipPath>
        <rect width="16" height="16" clip-path="url(#a)" fill="white" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ....,+%@@%+,....
        ...+@@@@@@@@+...
        ..%@@@@@@@@@@%..
        .+@@@@@@@@@@@@+.
        ,@@@@@@@@@@@@@@,
        +@@@@@@@@@@@@@@+
        %@@@@@@@@@@@@@@%
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        %@@@@@@@@@@@@@@%
        +@@@@@@@@@@@@@@+
        ,@@@@@@@@@@@@@@,
        .+@@@@@@@@@@@@+.
        ..%@@@@@@@@@@%..
        ...+@@@@@@@@+...
        ....,+%@@%+,....
        )"));
}

TEST(SVGClipPathElementTests, RenderingObjectBoundingBox) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <clipPath id="a" clipPathUnits="objectBoundingBox">
          <circle cx="0.5" cy="0.5" r="0.5" />
        </clipPath>
        <rect width="8" height="8" clip-path="url(#a)" fill="white" />
        <rect y="8" width="16" height="8" clip-path="url(#a)" fill="white" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        .,#%%#,.........
        ,@@@@@@,........
        #@@@@@@#........
        @@@@@@@%........
        @@@@@@@%........
        #@@@@@@#........
        ,@@@@@@,........
        .,#%%#,.........
        ...-+#@@@@#+-...
        .-%@@@@@@@@@@%-.
        -@@@@@@@@@@@@@@-
        @@@@@@@@@@@@@@@%
        @@@@@@@@@@@@@@@%
        -@@@@@@@@@@@@@@-
        .-%@@@@@@@@@@%-.
        ...-+#@@@@#+-...
        )"));
}

}  // namespace donner::svg
