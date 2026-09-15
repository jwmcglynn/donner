#include "donner/gpu/shader/wgsl/Number.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <locale>
#include <sstream>
#include <string>

namespace donner::gpu::shader::wgsl::number {
namespace {

TEST(Number, ParsesShaderLiteralKindsAndExactBits) {
  struct Case {
    const char* text;
    Kind kind;
    uint64_t bits;
  };
  for (const auto& item :
       {Case{"0", Kind::AbstractInt, 0}, Case{"2147483647i", Kind::I32, 0x7fffffff},
        Case{"0xFFFFFFFFu", Kind::U32, 0xffffffff},
        Case{"9223372036854775807", Kind::AbstractInt, 0x7fffffffffffffff},
        Case{"0.7071068f", Kind::F32, 0x3f3504f4}, Case{"3.402823466e38f", Kind::F32, 0x7f7fffff},
        Case{"0x1p-149f", Kind::F32, 1}, Case{"0x1p-1074", Kind::AbstractFloat, 1},
        Case{"0x1.fffffffffffffp1023", Kind::AbstractFloat, 0x7fefffffffffffff}}) {
    SCOPED_TRACE(item.text);
    const Value value = Parse(item.text);
    ASSERT_EQ(value.error, Error::None);
    EXPECT_EQ(value.kind, item.kind);
    EXPECT_EQ(value.bits, item.bits);
  }
}

TEST(Number, RejectsInvalidGrammarAndUnrepresentableValues) {
  for (const char* text :
       {"", "01", "01u", "0x", "0xg", "1e", "1e+", "1.2.3", "1.0u", "1h", "4294967296u",
        "2147483648i", "9223372036854775808", "0x10000000000000000", "1e309", "3.402823467e38f",
        "0x1.00000001p0f", "0x1p-150f"}) {
    SCOPED_TRACE(text);
    EXPECT_NE(Parse(text).error, Error::None);
  }
  EXPECT_EQ(Parse(std::string(257, '1')).error, Error::Capacity);
}

void ExpectFiniteBits(FloatResult result, uint64_t bits) {
  ASSERT_EQ(result.error, Error::None);
  EXPECT_EQ(result.bits, bits);
}

TEST(Number, RoundsTiesToEvenAcrossNormalAndSubnormalBoundaries) {
  ExpectFiniteBits(Convert(0x3ff0000010000000, 53, 24), 0x3f800000);
  ExpectFiniteBits(Convert(0x3ff0000030000000, 53, 24), 0x3f800002);
  ExpectFiniteBits(Round(UInt(1), UInt(2), -149, 24), 0);
  ExpectFiniteBits(Round(UInt(3), UInt(2), -149, 24), 2);
  ExpectFiniteBits(Round(UInt(0xffffff), UInt(2), -149, 24), 0x800000);
  ExpectFiniteBits(Round(UInt(1), UInt(2), -1074, 53), 0);
  ExpectFiniteBits(Round(UInt(3), UInt(2), -1074, 53), 2);
  ExpectFiniteBits(Round(UInt(0x1fffffffffffff), UInt(2), -1074, 53), 0x10000000000000);
  EXPECT_EQ(Round(UInt(0x1ffffff), UInt(2), 104, 24).error, Error::Range);
}

TEST(Number, RejectsNonfiniteInputsAndArithmeticCapacityOverflow) {
  EXPECT_EQ(Evaluate(Op::Divide, 0x3f800000, 0, 24).error, Error::DivisionByZero);
  EXPECT_NE(Evaluate(Op::Add, 0x7ff0000000000000, 0, 53).error, Error::None);
  EXPECT_NE(Convert(0x7fc00000, 24, 53).error, Error::None);
  EXPECT_NE(Convert(0x100000000, 24, 53).error, Error::None);
  EXPECT_NE(Round(UInt(1), UInt(1), 0, 0).error, Error::None);
  UInt small(1), wide(1);
  wide.shiftLeft(32);
  small.subtract(wide);
  EXPECT_EQ(small.valid, false);
  UInt denominator(1);
  denominator.shiftLeft(96);
  UInt numerator = denominator.multiply(UInt(UINT64_MAX));
  UInt half = denominator;
  half.shiftRightOne();
  numerator.add(half);
  numerator.addSmall(1);
  EXPECT_EQ(Divide(numerator, denominator).error, Error::Capacity);
}

TEST(Number, EvaluatesTheSlugAbstractFractionDuringConstantEvaluation) {
  constexpr auto value = Evaluate(Op::Divide, Parse("1.0").bits, Parse("65536.0").bits, 53);
  static_assert(value.error == Error::None && value.bits == 0x3ef0000000000000);
  EXPECT_EQ(value.bits, 0x3ef0000000000000);
}

TEST(Number, DecimalConversionAgreesWithStandardLibraryForBoundedSamples) {
  uint32_t random = 17;
  for (unsigned i = 0; i < 1000; ++i) {
    random = random * 1664525u + 1013904223u;
    const uint32_t significand = random % 999999999u + 1;
    random = random * 1664525u + 1013904223u;
    const int exponent = int(random % 51) - 25;
    const std::string text = std::to_string(significand) + "e" + std::to_string(exponent);
    SCOPED_TRACE(text);
    double reference64 = 0;
    float reference32 = 0;
    std::istringstream stream64(text), stream32(text);
    stream64.imbue(std::locale::classic());
    stream32.imbue(std::locale::classic());
    stream64 >> reference64;
    stream32 >> reference32;
    ASSERT_EQ(stream64.fail(), false);
    ASSERT_EQ(stream32.fail(), false);
    ASSERT_EQ(stream64.peek(), std::char_traits<char>::eof());
    ASSERT_EQ(stream32.peek(), std::char_traits<char>::eof());
    const Value actual64 = Parse(text), actual32 = Parse(text + "f");
    ASSERT_EQ(actual64.error, Error::None);
    ASSERT_EQ(actual32.error, Error::None);
    EXPECT_EQ(actual64.bits, std::bit_cast<uint64_t>(reference64));
    EXPECT_EQ(actual32.bits, std::bit_cast<uint32_t>(reference32));
  }
}

template <typename Float, typename Bits>
void CheckArithmetic(uint32_t precision) {
  uint32_t random = 31;
  for (unsigned i = 0; i < 500; ++i) {
    random = random * 1664525u + 1013904223u;
    const Float left = Float(std::bit_cast<int32_t>(random)) / Float(17);
    random = random * 1664525u + 1013904223u;
    const Float right = Float(std::bit_cast<int32_t>(random)) / Float(31);
    for (Op op : {Op::Add, Op::Subtract, Op::Multiply, Op::Divide}) {
      SCOPED_TRACE(i);
      SCOPED_TRACE(unsigned(op));
      volatile Float a = left, b = right;
      const Float reference = op == Op::Add        ? a + b
                              : op == Op::Subtract ? a - b
                              : op == Op::Multiply ? a * b
                                                   : a / b;
      ASSERT_EQ(std::isfinite(reference), true);
      const auto actual =
          Evaluate(op, std::bit_cast<Bits>(left), std::bit_cast<Bits>(right), precision);
      ASSERT_EQ(actual.error, Error::None);
      EXPECT_EQ(actual.bits, std::bit_cast<Bits>(reference));
    }
  }
}

TEST(Number, Binary32ArithmeticAgreesWithRuntimeReference) {
  CheckArithmetic<float, uint32_t>(24);
}
TEST(Number, Binary64ArithmeticAgreesWithRuntimeReference) {
  CheckArithmetic<double, uint64_t>(53);
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl::number
