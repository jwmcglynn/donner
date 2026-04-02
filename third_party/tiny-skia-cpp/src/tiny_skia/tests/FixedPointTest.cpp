#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

#include "tiny_skia/FixedPoint.h"

using ::testing::ElementsAre;

TEST(FixedPointFdot6Test, ConversionsAndRounding) {
  const std::array fromI32{tiny_skia::fdot6::fromI32(3), tiny_skia::fdot6::fromI32(-2)};
  EXPECT_THAT(fromI32, ElementsAre(192, -128));

  const std::array fromF32{tiny_skia::fdot6::fromF32(1.5f), tiny_skia::fdot6::fromF32(-1.25f)};
  EXPECT_THAT(fromF32, ElementsAre(96, -80));

  const std::array floorCeilRoundNeg{tiny_skia::fdot6::floor(-65), tiny_skia::fdot6::ceil(-65),
                                     tiny_skia::fdot6::round(-65)};
  EXPECT_THAT(floorCeilRoundNeg, ElementsAre(-2, -1, -1));

  const std::array floorCeilRoundPos{tiny_skia::fdot6::floor(65), tiny_skia::fdot6::ceil(65),
                                     tiny_skia::fdot6::round(31)};
  EXPECT_THAT(floorCeilRoundPos, ElementsAre(1, 2, 0));

  const std::array derivedFdot16{
      tiny_skia::fdot6::toFdot16(64),
      tiny_skia::fdot6::div(64, 32),
  };
  EXPECT_THAT(derivedFdot16, ElementsAre(65536, 131072));

  const std::array smallScale{
      tiny_skia::fdot6::smallScale(255, 64),
      tiny_skia::fdot6::smallScale(255, 32),
  };
  EXPECT_THAT(smallScale, ElementsAre(255, 127));

  struct CanConvertCase {
    std::string_view label;
    std::int32_t value;
    bool expected;
  };
  for (const auto& tc : {
           CanConvertCase{"safe bound", 30000, true},
           CanConvertCase{"safe max", std::numeric_limits<std::int32_t>::max() >> 10, true},
           CanConvertCase{"overflow bound", (std::numeric_limits<std::int32_t>::max() >> 10) + 1,
                          false},
           CanConvertCase{"overflow lower", std::numeric_limits<std::int32_t>::min(), false},
       }) {
    SCOPED_TRACE(tc.label);
    EXPECT_EQ(tiny_skia::fdot6::canConvertToFdot16(tc.value), tc.expected);
  }

  // Trigger the slow path when left-shifted fdot6 does not fit int16.
  const auto big = tiny_skia::fdot6::fromF32(3000.0f);
  EXPECT_EQ(tiny_skia::fdot16::divide(big, 64), 196608000);
}

TEST(FixedPointFdot6And8Test, Fdot8ConversionAndBoundaries) {
  const std::array converted{tiny_skia::fdot8::fromFdot16(0xFF00),
                             tiny_skia::fdot8::fromFdot16(-0x0100),
                             tiny_skia::fdot8::fromFdot16(-0x7F00)};
  EXPECT_THAT(converted, ElementsAre(0x0FF, static_cast<tiny_skia::FDot8>(-0x01), -0x7F));
}

TEST(FixedPointFdot16Test, FloatingConversionAndArithmetic) {
  const std::array fromF32{
      tiny_skia::fdot16::fromF32(1.0f),
      tiny_skia::fdot16::fromF32(-1.0f),
      tiny_skia::fdot16::fromF32(40000.0f),
      tiny_skia::fdot16::fromF32(-40000.0f),
  };
  EXPECT_THAT(fromF32, ElementsAre(65536, -65536, 2147483520, -2147483520));

  const std::array rounding{
      tiny_skia::fdot16::floorToI32(65535), tiny_skia::fdot16::floorToI32(-65535),
      tiny_skia::fdot16::ceilToI32(65535),  tiny_skia::fdot16::ceilToI32(-65535),
      tiny_skia::fdot16::roundToI32(98304), tiny_skia::fdot16::roundToI32(-98304),
  };
  EXPECT_THAT(rounding, ElementsAre(0, -1, 1, 0, 2, -1));

  const std::array arithmetic{
      tiny_skia::fdot16::mul(65536, 65536), tiny_skia::fdot16::mul(65536, -32768),
      tiny_skia::fdot16::divide(65536, 1),  tiny_skia::fdot16::divide(65536, -1),
      tiny_skia::fdot16::fastDiv(64, 32),   tiny_skia::fdot16::fastDiv(-64, 32),
  };
  EXPECT_THAT(arithmetic, ElementsAre(65536, -32768, std::numeric_limits<std::int32_t>::max(),
                                      std::numeric_limits<std::int32_t>::min(), 131072, -131072));

  const std::array saturating{
      tiny_skia::fdot16::divide(std::numeric_limits<tiny_skia::FDot6>::max(), 1),
      tiny_skia::fdot16::divide(std::numeric_limits<tiny_skia::FDot6>::min(), 1),
  };
  EXPECT_THAT(saturating, ElementsAre(std::numeric_limits<std::int32_t>::max(),
                                      std::numeric_limits<std::int32_t>::min()));
}

TEST(Fdot16AndFdot8, DivisionAndRoundingEdgeCases) {
  const std::array division{
      tiny_skia::fdot16::divide(0, 1),
      tiny_skia::fdot16::fastDiv(1, 1),
      tiny_skia::fdot16::fastDiv(0, 255),
  };
  EXPECT_THAT(division, ElementsAre(0, 65536, 0));

  struct CeilBoundaryCase {
    std::int32_t fixed;
    std::int32_t expected;
  };
  for (const auto& tc : {
           CeilBoundaryCase{1, 1},
           CeilBoundaryCase{65535, 1},
           CeilBoundaryCase{-65535, 0},
       }) {
    EXPECT_EQ(tiny_skia::fdot16::ceilToI32(tc.fixed), tc.expected);
  }
}

TEST(Fdot16AndFdot8, OneAndConstants) { EXPECT_EQ(tiny_skia::fdot16::one, 65536); }
