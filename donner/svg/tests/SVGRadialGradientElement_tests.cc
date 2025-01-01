#include "donner/svg/SVGRadialGradientElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGLinearGradientElement.h"
#include "donner/svg/core/Gradient.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;

namespace donner::svg {

TEST(SVGRadialGradientElementTests, Defaults) {
  auto gradient = instantiateSubtreeElementAs<SVGRadialGradientElement>("<radialGradient />");
  EXPECT_THAT(gradient->cx(), testing::Eq(std::nullopt));
  EXPECT_THAT(gradient->r(), testing::Eq(std::nullopt));
  EXPECT_THAT(gradient->fx(), testing::Eq(std::nullopt));
  EXPECT_THAT(gradient->fy(), testing::Eq(std::nullopt));
  EXPECT_THAT(gradient->fr(), testing::Eq(std::nullopt));

  EXPECT_THAT(gradient->href(), testing::Eq(std::nullopt));
  EXPECT_THAT(gradient->gradientUnits(), testing::Eq(GradientUnits::ObjectBoundingBox));
  EXPECT_THAT(gradient->gradientTransform(), TransformEq(Transformd()));
  EXPECT_THAT(gradient->spreadMethod(), testing::Eq(GradientSpreadMethod::Pad));
}

TEST(SVGRadialGradientElementTests, Cast) {
  auto gradient = instantiateSubtreeElementAs<SVGRadialGradientElement>("<radialGradient />");
  EXPECT_THAT(gradient->tryCast<SVGElement>(), testing::Ne(std::nullopt));
  EXPECT_THAT(gradient->tryCast<SVGGradientElement>(), testing::Ne(std::nullopt));
  EXPECT_THAT(gradient->tryCast<SVGRadialGradientElement>(), testing::Ne(std::nullopt));
  EXPECT_THAT(gradient->tryCast<SVGLinearGradientElement>(), testing::Eq(std::nullopt));
}

TEST(SVGRadialGradientElementTests, RenderingDefaults) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <radialGradient id="a">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </radialGradient>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        .....,,,,,,.....
        ...,,::--::,,...
        ..,::-====-::,..
        ..,:-=++++=-:,..
        .,:-=+****+=-:,.
        .,:=+*#%%#*+=:,.
        .,-=+*%@@%*+=-,.
        .,-=+*%@@%*+=-,.
        .,:=+*#%%#*+=:,.
        .,:-=+****+=-:,.
        ..,:-=++++=-:,..
        ..,::-====-::,..
        ...,,::--::,,...
        .....,,,,,,.....
        ................
        )"));
}

TEST(SVGRadialGradientElementTests, GradientCoordinates) {
  ParsedFragment<SVGRadialGradientElement> fragment =
      instantiateSubtreeElementAs<SVGRadialGradientElement>(R"-(
        <radialGradient id="a" cx="42.5%" cy="62.5%" r="87.5%" fx="62.5%" fy="42.5%" fr="12.5%">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </radialGradient>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_THAT(fragment->cx(), testing::Optional(LengthIs(42.5, Lengthd::Unit::Percent)));
  EXPECT_THAT(fragment->cy(), testing::Optional(LengthIs(62.5, Lengthd::Unit::Percent)));
  EXPECT_THAT(fragment->r(), testing::Optional(LengthIs(87.5, Lengthd::Unit::Percent)));
  EXPECT_THAT(fragment->fx(), testing::Optional(LengthIs(62.5, Lengthd::Unit::Percent)));
  EXPECT_THAT(fragment->fy(), testing::Optional(LengthIs(42.5, Lengthd::Unit::Percent)));
  EXPECT_THAT(fragment->fr(), testing::Optional(LengthIs(12.5, Lengthd::Unit::Percent)));

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        ::--===+++==--:,
        --==+++****++=-:
        -==++**####**+=-
        ==+**##%%%%%#*+=
        =++*##%@@@@@%#*=
        =+**#%%@@@@@@#*+
        ++*##%@@@@@@@%#+
        ++*##%@@@@@@@%#*
        ++*##%@@@@@@@%#*
        ++*##%%@@@@@%##*
        ++**##%%%@@%%#*+
        =++**##%%%%%#**+
        =++***######**+=
        ==++****##***++=
        -==+++*****+++=-
        --==++++++++==--
        )"));
  }

  fragment->setFx(Lengthd(50, Lengthd::Unit::Percent));
  fragment->setFy(Lengthd(50, Lengthd::Unit::Percent));

  // Verify that the properties are updated.
  EXPECT_THAT(fragment->fx(), testing::Optional(LengthIs(50, Lengthd::Unit::Percent)));
  EXPECT_THAT(fragment->fy(), testing::Optional(LengthIs(50, Lengthd::Unit::Percent)));

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
    EXPECT_TRUE(generatedAscii.matches(R"(
        ::--=======--:,,
        --==++++++==--:,
        -==++*****++==-:
        =++**#####**+==-
        =+**#%%%%%##*+=-
        +**#%%@@@@%#**+=
        +*##%@@@@@@%#*+=
        +*#%%@@@@@@%#*+=
        +*#%@@@@@@@%#*++
        +*#%%@@@@@@%#*++
        +*##%@@@@@%%#*+=
        +**#%%%@@%%##*+=
        ++*###%%%%##*++=
        =+**#######**+==
        =++*********+==-
        ==++++***+++==--
        )"));
  }
}

TEST(SVGRadialGradientElementTests, GradientUnitsUserSpaceOnUse) {
  auto gradient = instantiateSubtreeElementAs<SVGRadialGradientElement>(
      R"(<radialGradient gradientUnits="userSpaceOnUse" />")");
  EXPECT_THAT(gradient->gradientUnits(), testing::Eq(GradientUnits::UserSpaceOnUse));
}

TEST(SVGRadialGradientElementTests, GradientUnitsObjectBoundingBox) {
  auto gradient = instantiateSubtreeElementAs<SVGRadialGradientElement>(
      R"(<radialGradient gradientUnits="objectBoundingBox" />")");
  EXPECT_THAT(gradient->gradientUnits(), testing::Eq(GradientUnits::ObjectBoundingBox));
}
TEST(SVGRadialGradientElementTests, GradientUnitsRendering) {
  ParsedFragment<SVGRadialGradientElement> fragment =
      instantiateSubtreeElementAs<SVGRadialGradientElement>(R"-(
        <radialGradient id="a" gradientUnits="userSpaceOnUse" cx="10" cy="10" r="8" fx="8" fx="8" fr="4">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </radialGradient>
        <rect x="0" y="0" width="8" height="8" fill="url(#a)" />
        <rect x="8" y="8" width="8" height="8" fill="url(#a)" />
        )-");

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ......:-........
        .....-=+........
        ....-*#%........
        ...:*@@@........
        ...=%@@@........
        ........@@@@%#+-
        ........@@@@@#+=
        ........@@@@@#+=
        ........@@@@%#+-
        ........@@@@%*+-
        ........@@@%#+=:
        ........%%#*+=-,
        ........**++=:,.
        )"));
  }

  // Change gradientUnits, rendering should change.
  fragment->setGradientUnits(GradientUnits::ObjectBoundingBox);

  EXPECT_EQ(fragment->gradientUnits(), GradientUnits::ObjectBoundingBox);

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
    EXPECT_TRUE(generatedAscii.matches(R"(
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
        )"));
  }
}

TEST(SVGRadialGradientElementTests, RenderingTransform) {
  ParsedFragment<SVGRadialGradientElement> fragment =
      instantiateSubtreeElementAs<SVGRadialGradientElement>(R"-(
        <radialGradient id="a" gradientTransform="translate(0.5 0.5) rotate(45) scale(1 2) translate(-0.5 -0.5)">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </radialGradient>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        ......,::--===--
        ....,,:--======-
        ...,::-==+++++==
        ..,::-=++****+==
        .,::-=+**###*+==
        .,:-=+*##%##*+=-
        ,:-=+*#%%%%#*+=-
        :-=+*#%@@%#*+=-:
        :-=+*#%@@%#*+=-:
        -=+*#%%%%#*+=-:,
        -=+*##%##*+=-:,.
        ==+*###**+=-::,.
        ==+****++=-::,..
        ==+++++==-::,...
        -======--:,,....
        --===--::,......
        )"));
  }

  fragment->setGradientTransform(Transformd());

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        .....,,,,,,.....
        ...,,::--::,,...
        ..,::-====-::,..
        ..,:-=++++=-:,..
        .,:-=+****+=-:,.
        .,:=+*#%%#*+=:,.
        .,-=+*%@@%*+=-,.
        .,-=+*%@@%*+=-,.
        .,:=+*#%%#*+=:,.
        .,:-=+****+=-:,.
        ..,:-=++++=-:,..
        ..,::-====-::,..
        ...,,::--::,,...
        .....,,,,,,.....
        ................
        )"));
  }
}

TEST(SVGRadialGradientElementTests, SpreadMethodPad) {
  auto gradient = instantiateSubtreeElementAs<SVGRadialGradientElement>(
      R"(<radialGradient spreadMethod="pad" />)");
  EXPECT_THAT(gradient->spreadMethod(), testing::Eq(GradientSpreadMethod::Pad));
}

TEST(SVGRadialGradientElementTests, SpreadMethodReflect) {
  auto gradient = instantiateSubtreeElementAs<SVGRadialGradientElement>(
      R"(<radialGradient spreadMethod="reflect" />)");
  EXPECT_THAT(gradient->spreadMethod(), testing::Eq(GradientSpreadMethod::Reflect));
}

TEST(SVGRadialGradientElementTests, SpreadMethodRepeat) {
  auto gradient = instantiateSubtreeElementAs<SVGRadialGradientElement>(
      R"(<radialGradient spreadMethod="repeat" />)");
  EXPECT_THAT(gradient->spreadMethod(), testing::Eq(GradientSpreadMethod::Repeat));
}

TEST(SVGRadialGradientElementTests, SpreadMethodRendering) {
  ParsedFragment<SVGRadialGradientElement> fragment =
      instantiateSubtreeElementAs<SVGRadialGradientElement>(R"-(
        <radialGradient id="a" spreadMethod="pad" cx="42.5%" cy="62.5%" r="87.5%" fx="62.5%" fy="42.5%" fr="25%">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </radialGradient>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        :-==++******+=-:
        -=++**##%%%#*+=-
        =++*##%@@@@@%#*=
        =+*##%@@@@@@@%#+
        +**#%@@@@@@@@@%*
        +*#%%@@@@@@@@@@#
        +*#%@@@@@@@@@@@#
        +*#%@@@@@@@@@@@#
        +*#%%@@@@@@@@@@#
        +*#%%@@@@@@@@@%#
        +*##%%@@@@@@@@%#
        +**##%%@@@@@@%#*
        ++**##%%%%%%%#**
        =++**###%%%##**+
        ==++**######**+=
        -==++*******++==
        )"));
  }

  // Change spreadMethod to reflect, rendering should change.
  fragment->setSpreadMethod(GradientSpreadMethod::Reflect);

  EXPECT_EQ(fragment->spreadMethod(), GradientSpreadMethod::Reflect);

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
    EXPECT_TRUE(generatedAscii.matches(R"(
        :-==++******+=-:
        -=++**##%%%#*+=-
        =++*##%@@@@@%#*=
        =+*##%@@@@@@@%#+
        +**#%@@@%%##@@%*
        +*#%%@@@%#**%@@#
        +*#%@@@@%###%@@#
        +*#%@@@@%%##%@@#
        +*#%%@@@@%%%@@@#
        +*#%%@@@@@@@@@%#
        +*##%%@@@@@@@@%#
        +**##%%@@@@@@%#*
        ++**##%%%%%%%#**
        =++**###%%%##**+
        ==++**######**+=
        -==++*******++==
        )"));
  }

  // Change spreadMethod to repeat, rendering should change.
  fragment->setSpreadMethod(GradientSpreadMethod::Repeat);

  EXPECT_EQ(fragment->spreadMethod(), GradientSpreadMethod::Repeat);

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
    EXPECT_TRUE(generatedAscii.matches(R"(
        :-==++******+=-:
        -=++**##%%%#*+=-
        =++*##%@@@@@%#*=
        =+*##%@@..,.@%#+
        +**#%@@.,:::,@%*
        +*#%%@.,,:--:.@#
        +*#%@@.,,:--:.@#
        +*#%@@.,,:::,.@#
        +*#%%@..,,,,,.@#
        +*#%%@@...,..@%#
        +*##%%@@....@@%#
        +**##%%@@@@@@%#*
        ++**##%%%%%%%#**
        =++**###%%%##**+
        ==++**######**+=
        -==++*******++==
        )"));
  }
}

TEST(SVGRadialGradientElementTests, HrefSimple) {
  auto gradient = instantiateSubtreeElementAs<SVGRadialGradientElement>(
      R"(<radialGradient href="#refGradient" />)");
  EXPECT_THAT(gradient->href(), testing::Optional(testing::Eq("#refGradient")));
}

TEST(SVGRadialGradientElementTests, HrefInheritanceChildrenXYRendering) {
  ParsedFragment<SVGRadialGradientElement> fragment =
      instantiateSubtreeElementAs<SVGRadialGradientElement>(R"-(
        <radialGradient id="gradient" href="#refGradient" />
        <radialGradient id="refGradient" cx="10%" cy="20%" r="80%">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </radialGradient>
        <rect width="16" height="16" fill="url(#gradient)" />
        )-");

  EXPECT_THAT(fragment->href(), testing::Optional(testing::Eq("#refGradient")));
  EXPECT_THAT(fragment->cx(), testing::Eq(std::nullopt));
  EXPECT_THAT(fragment->cy(), testing::Eq(std::nullopt));
  EXPECT_THAT(fragment->r(), testing::Eq(std::nullopt));

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        #%###*++=-::,...
        %%%%#**+=-::,...
        @@@%##*+=--:,...
        @@@%##*+=--:,...
        %@%%#**+=--:,...
        %%%##*++=-::,...
        ####**+==-:,,...
        *****++=--:,....
        +++++==--:,,....
        +++===--::,.....
        ====---::,,.....
        -----:::,,......
        ::::::,,........
        ,:,,,,,.........
        ,,,,............
        ................
        )"));
  }
}

TEST(SVGRadialGradientElementTests, HrefInheritanceSharedParamsRendering) {
  ParsedFragment<SVGRadialGradientElement> fragment =
      instantiateSubtreeElementAs<SVGRadialGradientElement>(R"-(
        <radialGradient id="gradient" href="#refGradient" gradientUnits="userSpaceOnUse"
            gradientTransform="rotate(90)" spreadMethod="repeat">
          <!-- should be overridden -->
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </radialGradient>
        <radialGradient id="refGradient" cx="10%" cy="20%" r="80%">
          <stop offset="20%" stop-color="white" />
          <stop offset="80%" stop-color="black" />
        </radialGradient>
        <rect width="16" height="16" fill="url(#gradient)" />
        )-");

  EXPECT_THAT(fragment->href(), testing::Optional(testing::Eq("#refGradient")));
  EXPECT_THAT(fragment->cx(), testing::Eq(std::nullopt));
  EXPECT_THAT(fragment->cy(), testing::Eq(std::nullopt));
  EXPECT_THAT(fragment->r(), testing::Eq(std::nullopt));

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        #*+==-:,..@%##*+
        #*+==-:,..@%##*+
        #*+==-:,..@%##*+
        **+=--:,.@@%##*+
        *++=-::,.@@%#**+
        ++==-:,,.@@%#*++
        +==-::,..@%%#*++
        ==--:,,.@@%##*+=
        ---:,,..@%%#**+=
        -::,,..@@%##*+==
        ::,,..@@%%#**+=-
        ,,,..@@%%#**+==-
        ,...@@%%##*++=--
        ..@@@%%##*++==-:
        @@@%%%##*++==-::
        %%%%##**++==--:,
        )"));
  }
}

}  // namespace donner::svg
