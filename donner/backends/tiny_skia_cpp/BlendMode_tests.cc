#include "donner/backends/tiny_skia_cpp/BlendMode.h"

#include <gtest/gtest.h>

#include <cstdlib>

namespace donner::backends::tiny_skia_cpp {
namespace {

PremultipliedColorF BlendPremult(Color source, Color dest, BlendMode mode) {
  return Blend(Premultiply(source), Premultiply(dest), mode);
}

void ExpectColorNear(const Color& actual, const Color& expected, int tolerance = 1) {
  EXPECT_LE(std::abs(static_cast<int>(actual.r) - static_cast<int>(expected.r)), tolerance);
  EXPECT_LE(std::abs(static_cast<int>(actual.g) - static_cast<int>(expected.g)), tolerance);
  EXPECT_LE(std::abs(static_cast<int>(actual.b) - static_cast<int>(expected.b)), tolerance);
  EXPECT_LE(std::abs(static_cast<int>(actual.a) - static_cast<int>(expected.a)), tolerance);
}

TEST(BlendModeTests, PorterDuffBasics) {
  const Color source(220, 140, 75, 180);
  const Color dest(50, 127, 150, 200);

  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kClear)), Color(0, 0, 0, 0));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kDestination)),
                  Color(39, 100, 118, 200));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kSourceOver)),
                  Color(167, 128, 88, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kDestinationOver)),
                  Color(72, 121, 129, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kSourceIn)),
                  Color(122, 78, 42, 141));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kDestinationIn)),
                  Color(28, 71, 83, 141));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kSourceOut)),
                  Color(33, 21, 11, 39));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kDestinationOut)),
                  Color(11, 29, 35, 59));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kSourceAtop)),
                  Color(133, 107, 76, 200));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kDestinationAtop)),
                  Color(61, 92, 95, 180));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kXor)), Color(45, 51, 46, 98));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kPlus)), Color(194, 199, 171, 255));
}

TEST(BlendModeTests, AdvancedBlendModes) {
  const Color source(220, 140, 75, 180);
  const Color dest(50, 127, 150, 200);

  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kModulate)),
                  Color(24, 39, 24, 141));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kScreen)),
                  Color(171, 160, 146, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kOverlay)),
                  Color(92, 128, 106, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kDarken)), Color(72, 121, 88, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kLighten)),
                  Color(167, 128, 129, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kColorDodge)),
                  Color(186, 192, 164, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kColorBurn)),
                  Color(54, 63, 46, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kHardLight)),
                  Color(155, 128, 95, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kSoftLight)),
                  Color(98, 124, 115, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kDifference)),
                  Color(139, 58, 88, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kExclusion)),
                  Color(147, 121, 122, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kMultiply)),
                  Color(69, 89, 71, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kHue)), Color(128, 103, 74, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kSaturation)),
                  Color(59, 126, 140, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kColor)), Color(139, 100, 60, 239));
  ExpectColorNear(ToColor(BlendPremult(source, dest, BlendMode::kLuminosity)),
                  Color(100, 149, 157, 239));
}

}  // namespace
}  // namespace donner::backends::tiny_skia_cpp
