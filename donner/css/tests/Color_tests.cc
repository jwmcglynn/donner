#include "donner/css/Color.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::css {

using ::testing::Eq;
using ::testing::Ne;
using ::testing::Optional;

/// @test \ref RGBA::toHexString for various colors.
TEST(RGBA, ToHexString) {
  EXPECT_THAT(RGBA().toHexString(), "#ffffff");
  EXPECT_THAT(RGBA(0, 0, 0, 0).toHexString(), "#00000000");
  EXPECT_THAT(RGBA(0, 0, 0, 255).toHexString(), "#000000");
  EXPECT_THAT(RGBA(255, 255, 255, 255).toHexString(), "#ffffff");
  EXPECT_THAT(RGBA(255, 255, 255, 0).toHexString(), "#ffffff00");
  EXPECT_THAT(RGBA(0, 0, 0, 128).toHexString(), "#00000080");
  EXPECT_THAT(RGBA(255, 0, 0, 255).toHexString(), "#ff0000");
}

/// @test \ref Color::operator==
TEST(Color, OperatorEquality) {
  // Same RGBA
  EXPECT_TRUE(Color(RGBA(0, 0, 0, 255)) == Color(RGBA(0, 0, 0, 255)));
  EXPECT_FALSE(Color(RGBA(0, 0, 0, 255)) == Color(RGBA(1, 0, 0, 255)));

  // RGBA vs HSLA are never "==" unless they store the exact same variant & values.
  // Even if visually identical, the variant types differ.
  EXPECT_FALSE(Color(RGBA(255, 0, 0, 255)) == Color(HSLA::HSL(0, 1.0f, 0.5f)));

  // Same HSLA
  EXPECT_TRUE(Color(HSLA::HSL(120, 0.5f, 0.5f)) == Color(HSLA::HSL(120, 0.5f, 0.5f)));
  EXPECT_FALSE(Color(HSLA::HSL(120, 0.5f, 0.5f)) == Color(HSLA::HSL(120, 0.6f, 0.5f)));

  // currentColor vs currentColor
  EXPECT_TRUE(Color(Color::CurrentColor()) == Color(Color::CurrentColor()));
  EXPECT_FALSE(Color(Color::CurrentColor()) == Color(RGBA()));

  // RGBA to/from Color
  {
    Color rgbaColor(RGBA(10, 20, 30, 40));
    // Color == RGBA
    EXPECT_EQ(rgbaColor, RGBA(10, 20, 30, 40));
    EXPECT_NE(rgbaColor, RGBA(11, 20, 30, 40));
    // RGBA == Color (using the friend operator)
    EXPECT_EQ(RGBA(10, 20, 30, 40), rgbaColor);
    EXPECT_NE(RGBA(11, 20, 30, 40), rgbaColor);
  }

  // HSLA to/from Color
  {
    Color hslaColor(HSLA::HSL(120, 0.5f, 0.5f));
    // Color == HSLA
    EXPECT_EQ(hslaColor, HSLA::HSL(120, 0.5f, 0.5f));
    EXPECT_NE(hslaColor, HSLA::HSL(120, 0.6f, 0.5f));
    // HSLA == Color (using the friend operator)
    EXPECT_EQ(HSLA::HSL(120, 0.5f, 0.5f), hslaColor);
    EXPECT_NE(HSLA::HSL(120, 0.6f, 0.5f), hslaColor);
  }

  // currentColor comparisons
  {
    Color currentColor(Color::CurrentColor{});
    // Color == CurrentColor
    EXPECT_EQ(currentColor, Color::CurrentColor());
    // currentColor should not equal an RGBA or HSLA value.
    EXPECT_NE(currentColor, RGBA(255, 255, 255, 255));
    EXPECT_NE(currentColor, HSLA::HSL(0, 0.5f, 0.5f));
  }
}

/// @test \ref Color::ByName
TEST(Color, ByName) {
  // A known color
  auto maybeRed = Color::ByName("red");
  ASSERT_TRUE(maybeRed.has_value());
  EXPECT_TRUE(maybeRed->hasRGBA());
  EXPECT_EQ(maybeRed->rgba(), RGBA(255, 0, 0, 255));

  // currentcolor should parse
  auto maybeCurrent = Color::ByName("currentcolor");
  ASSERT_TRUE(maybeCurrent.has_value());
  EXPECT_TRUE(maybeCurrent->isCurrentColor());

  // Non-existent color
  EXPECT_FALSE(Color::ByName("thisColorDoesNotExist").has_value());
}

/// @test Color accessors, such as isCurrentColor, hasRGBA, rgba, hasHSLA, hsla
TEST(Color, Accessors) {
  {
    Color c(RGBA(10, 20, 30, 255));
    EXPECT_TRUE(c.hasRGBA());
    EXPECT_FALSE(c.hasHSLA());
    EXPECT_FALSE(c.isCurrentColor());
    EXPECT_EQ(c.rgba(), RGBA(10, 20, 30, 255));
  }
  {
    Color c(HSLA::HSL(120, 0.5f, 0.5f));
    EXPECT_FALSE(c.hasRGBA());
    EXPECT_TRUE(c.hasHSLA());
    EXPECT_FALSE(c.isCurrentColor());
    EXPECT_EQ(c.hsla(), HSLA::HSL(120, 0.5f, 0.5f));
  }
  {
    Color c(Color::CurrentColor{});
    EXPECT_FALSE(c.hasRGBA());
    EXPECT_FALSE(c.hasHSLA());
    EXPECT_TRUE(c.isCurrentColor());
  }
}

/// @test HSL conversion to RGBA
TEST(Color, asRGBA) {
  EXPECT_EQ(HSLA::HSL(0.0f, 0.5f, 0.1f).toRGBA(), RGBA(38, 13, 13, 255));
  EXPECT_EQ(HSLA::HSL(90.0f, 0.5f, 0.5f).toRGBA(), RGBA(128, 191, 64, 255));
  EXPECT_EQ(HSLA::HSL(180.0f, 0.5f, 0.5f).toRGBA(), RGBA(64, 191, 191, 255));
  EXPECT_EQ(HSLA::HSL(270.0f, 0.5f, 0.9f).toRGBA(), RGBA(230, 217, 242, 255));
  EXPECT_EQ(HSLA::HSL(360.0f, 0.9f, 0.3f).toRGBA(), RGBA(145, 8, 8, 255));

  // With degrees outside of 0-360
  EXPECT_EQ(HSLA::HSL(-90.0f, 0.5f, 0.9f).toRGBA(), RGBA(230, 217, 242, 255));
  EXPECT_EQ(HSLA::HSL(450.0f, 0.5f, 0.5f).toRGBA(), RGBA(128, 191, 64, 255));

  EXPECT_EQ(HSLA::HSL(120.0f, 0.5f, 0.5f).toRGBA(), RGBA(64, 191, 64, 255));
  EXPECT_EQ(HSLA::HSL(240.0f, 0.5f, 0.5f).toRGBA(), RGBA(64, 64, 191, 255));

  // No-op conversion if already RGBA
  {
    Color c(RGBA(10, 20, 30, 128));
    EXPECT_EQ(c.asRGBA(), RGBA(10, 20, 30, 128));
  }

  // Death test for currentColor
  EXPECT_DEATH(
      {
        Color c(Color::CurrentColor{});
        // This triggers an assertion from asRGBA()
        std::ignore = c.asRGBA();
      },
      "Cannot convert currentColor to RGBA");
}

/// @test Tests for \ref Color::resolve.
TEST(Color, Resolve) {
  // Resolving an RGBA color
  {
    Color c(RGBA(100, 150, 200, 128));
    // Opacity = 1 => same alpha
    EXPECT_EQ(c.resolve(RGBA(0, 0, 0, 255), 1.f), RGBA(100, 150, 200, 128));
    // Opacity = 0.5 => alpha halved
    EXPECT_EQ(c.resolve(RGBA(0, 0, 0, 255), 0.5f), RGBA(100, 150, 200, 64));
  }

  // Resolving currentColor
  {
    Color c(Color::CurrentColor{});
    // Must substitute the given "currentColor" and multiply alpha by the given opacity
    EXPECT_EQ(c.resolve(RGBA(10, 20, 30, 128), 1.f), RGBA(10, 20, 30, 128));
    EXPECT_EQ(c.resolve(RGBA(10, 20, 30, 128), 0.25f), RGBA(10, 20, 30, 32));
  }

  // Resolving HSLA
  {
    // HSLA(0, 1.0, 0.5) is red (#ff0000). Then alpha multiplied by opacity.
    Color c(HSLA::HSL(0, 1.0f, 0.5f));
    EXPECT_EQ(c.resolve(RGBA(0, 0, 0, 255), 1.f), RGBA(255, 0, 0, 255));
    EXPECT_EQ(c.resolve(RGBA(0, 0, 0, 255), 0.3f), RGBA(255, 0, 0, 76));
  }
}

/// @test Hex helper functions for constructing \ref Color.
TEST(Color, HexHelpers) {
  // RgbHex(0xFF0000) => red (opaque)
  EXPECT_EQ(RgbHex(0xFF0000), RGBA(255, 0, 0, 255));
  // RgbHex(0x0000FF) => blue (opaque)
  EXPECT_EQ(RgbHex(0x0000FF), RGBA(0, 0, 255, 255));
  // RgbaHex(0x00FF00FF) => green at 0xFF alpha (fully opaque green)
  EXPECT_EQ(RgbaHex(0x00FF00FF), RGBA(0, 255, 0, 255));
  // RgbaHex(0xFF000080) => red at half alpha
  EXPECT_EQ(RgbaHex(0xFF000080), RGBA(255, 0, 0, 128));
  // RgbaHex(0x11223344) => direct channel check
  EXPECT_EQ(RgbaHex(0x11223344), RGBA(0x11, 0x22, 0x33, 0x44));
}

/// @test Ostream output \c operator<< for \ref Color.
TEST(Color, OstreamOutput) {
  EXPECT_THAT(Color(RGBA()), ToStringIs("rgba(255, 255, 255, 255)"));
  EXPECT_THAT(Color(RGBA(0, 0, 0, 0)), ToStringIs("rgba(0, 0, 0, 0)"));
  EXPECT_THAT(Color(RGBA(0, 0, 0, 255)), ToStringIs("rgba(0, 0, 0, 255)"));
  EXPECT_THAT(Color(RGBA(255, 255, 255, 255)), ToStringIs("rgba(255, 255, 255, 255)"));
  EXPECT_THAT(Color(RGBA(255, 255, 255, 0)), ToStringIs("rgba(255, 255, 255, 0)"));
  EXPECT_THAT(Color(RGBA(0, 0, 0, 128)), ToStringIs("rgba(0, 0, 0, 128)"));
  EXPECT_THAT(Color(RGBA(255, 0, 0, 255)), ToStringIs("rgba(255, 0, 0, 255)"));
  EXPECT_THAT(Color(RGBA(0x11, 0x22, 0x33, 0x44)), ToStringIs("rgba(17, 34, 51, 68)"));

  // Test currentColor
  EXPECT_THAT(Color(Color::CurrentColor()), ToStringIs("currentColor"));

  // Test HSLA
  EXPECT_THAT(Color(HSLA::HSL(240, 1.0f, 0.5f)), ToStringIs("hsla(240, 100%, 50%, 255)"));

  EXPECT_THAT(RgbHex(0xFFFFFF), ToStringIs("rgba(255, 255, 255, 255)"));
  EXPECT_THAT(RgbHex(0x000000), ToStringIs("rgba(0, 0, 0, 255)"));
  EXPECT_THAT(RgbHex(0x123456), ToStringIs("rgba(18, 52, 86, 255)"));

  EXPECT_THAT(RgbaHex(0xFFFFFF00), ToStringIs("rgba(255, 255, 255, 0)"));
  EXPECT_THAT(RgbaHex(0x000000CC), ToStringIs("rgba(0, 0, 0, 204)"));
  EXPECT_THAT(RgbaHex(0x12345678), ToStringIs("rgba(18, 52, 86, 120)"));
}

}  // namespace donner::css
