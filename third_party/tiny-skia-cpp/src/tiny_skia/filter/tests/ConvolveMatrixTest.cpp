/// Literal SVG feConvolveMatrix bias expectations for byte and float paths.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>

#include "tiny_skia/filter/ConvolveMatrix.h"

namespace tiny_skia::filter {
namespace {

struct BiasCase {
  const char* name;
  double bias;
  bool preserveAlpha;
  std::array<float, 4> input;
  std::array<float, 4> expected;
  std::array<std::uint8_t, 4> byteInput;
  std::array<std::uint8_t, 4> byteExpected;
};

// Filter Effects 1, section 9.9: A = sumA/divisor + bias; RGB bias uses raw resulting A before
// clamping. The byte literals account independently for input quantization and nearest rounding.
constexpr std::array<BiasCase, 8> kBiasCases{{
    {"positive",
     0.25,
     false,
     {0.2f, 0.1f, 0.05f, 0.5f},
     {0.3875f, 0.2875f, 0.2375f, 0.75f},
     {51, 26, 13, 128},
     {99, 74, 61, 192}},
    {"negative",
     -0.25,
     false,
     {0.2f, 0.1f, 0.05f, 0.5f},
     {0.1375f, 0.0375f, 0.0f, 0.25f},
     {51, 26, 13, 128},
     {35, 10, 0, 64}},
    {"transparent",
     0.25,
     false,
     {0.0f, 0.0f, 0.0f, 0.0f},
     {0.0625f, 0.0625f, 0.0625f, 0.25f},
     {0, 0, 0, 0},
     {16, 16, 16, 64}},
    {"clamp_high",
     0.75,
     false,
     {0.2f, 0.1f, 0.05f, 0.5f},
     {1.0f, 1.0f, 0.9875f, 1.0f},
     {51, 26, 13, 128},
     {255, 255, 252, 255}},
    {"clamp_low",
     -0.75,
     false,
     {0.2f, 0.1f, 0.05f, 0.5f},
     {0.0f, 0.0f, 0.0f, 0.0f},
     {51, 26, 13, 128},
     {0, 0, 0, 0}},
    {"opaque",
     0.25,
     false,
     {0.2f, 0.1f, 0.05f, 1.0f},
     {0.5125f, 0.4125f, 0.3625f, 1.0f},
     {51, 26, 13, 255},
     {131, 106, 93, 255}},
    {"zero",
     0.0,
     false,
     {0.2f, 0.1f, 0.05f, 0.5f},
     {0.2f, 0.1f, 0.05f, 0.5f},
     {51, 26, 13, 128},
     {51, 26, 13, 128}},
    {"preserved",
     0.25,
     true,
     {0.2f, 0.1f, 0.05f, 0.5f},
     {0.325f, 0.225f, 0.175f, 0.5f},
     {51, 26, 13, 128},
     {83, 58, 45, 128}},
}};

TEST(ConvolveMatrixTest, FloatBiasUsesResultAlpha) {
  constexpr std::array<double, 1> kernel{1.0};
  for (const BiasCase& testCase : kBiasCases) {
    SCOPED_TRACE(testCase.name);
    auto input = FloatPixmap::fromSize(1, 1);
    auto output = FloatPixmap::fromSize(1, 1);
    ASSERT_THAT(input.has_value(), testing::IsTrue());
    ASSERT_THAT(output.has_value(), testing::IsTrue());
    std::copy(testCase.input.begin(), testCase.input.end(), input->data().begin());
    const ConvolveParams params{1,
                                1,
                                kernel,
                                1.0,
                                testCase.bias,
                                0,
                                0,
                                ConvolveEdgeMode::Duplicate,
                                testCase.preserveAlpha};
    convolveMatrix(*input, *output, params);
    EXPECT_THAT(output->data(), testing::Pointwise(testing::FloatNear(1e-6f), testCase.expected));
  }
}

TEST(ConvolveMatrixTest, ByteBiasUsesResultAlpha) {
  constexpr std::array<double, 1> kernel{1.0};
  for (const BiasCase& testCase : kBiasCases) {
    SCOPED_TRACE(testCase.name);
    auto input = Pixmap::fromSize(1, 1);
    auto output = Pixmap::fromSize(1, 1);
    ASSERT_THAT(input.has_value(), testing::IsTrue());
    ASSERT_THAT(output.has_value(), testing::IsTrue());
    std::copy(testCase.byteInput.begin(), testCase.byteInput.end(), input->data().begin());
    const ConvolveParams params{1,
                                1,
                                kernel,
                                1.0,
                                testCase.bias,
                                0,
                                0,
                                ConvolveEdgeMode::Duplicate,
                                testCase.preserveAlpha};
    convolveMatrix(*input, *output, params);
    EXPECT_THAT(output->data(), testing::ElementsAreArray(testCase.byteExpected));
  }
}

TEST(ConvolveMatrixTest, SubnormalDivisorAvoidsReciprocalOverflow) {
  for (const double coefficient : {1e-310, -1e-310}) {
    SCOPED_TRACE(coefficient);
    const std::array<double, 1> kernel{coefficient};
    const ConvolveParams params{
        1, 1, kernel, coefficient, 0.0, 0, 0, ConvolveEdgeMode::Duplicate, false};
    auto byteInput = Pixmap::fromSize(1, 1);
    auto byteOutput = Pixmap::fromSize(1, 1);
    auto floatInput = FloatPixmap::fromSize(1, 1);
    auto floatOutput = FloatPixmap::fromSize(1, 1);
    ASSERT_THAT(byteInput.has_value(), testing::IsTrue());
    ASSERT_THAT(byteOutput.has_value(), testing::IsTrue());
    ASSERT_THAT(floatInput.has_value(), testing::IsTrue());
    ASSERT_THAT(floatOutput.has_value(), testing::IsTrue());
    const std::array<std::uint8_t, 4> bytes{64, 128, 0, 255};
    const std::array<float, 4> floats{0.25f, 0.5f, 0.0f, 1.0f};
    std::copy(bytes.begin(), bytes.end(), byteInput->data().begin());
    std::copy(floats.begin(), floats.end(), floatInput->data().begin());
    convolveMatrix(*byteInput, *byteOutput, params);
    convolveMatrix(*floatInput, *floatOutput, params);
    EXPECT_THAT(byteOutput->data(), testing::ElementsAreArray(bytes));
    EXPECT_THAT(floatOutput->data(), testing::Pointwise(testing::FloatNear(1e-6f), floats));
  }
}

}  // namespace
}  // namespace tiny_skia::filter
