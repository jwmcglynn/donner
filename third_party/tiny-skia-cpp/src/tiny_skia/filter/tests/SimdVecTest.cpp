#include <array>
#include <bit>
#include <cstdint>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tiny_skia/filter/SimdVec.h"

namespace tiny_skia::filter {
namespace {

using ::testing::ElementsAreArray;

using Bytes = std::array<std::uint8_t, 4>;
using Floats = std::array<float, 4>;
using FloatBits = std::array<std::uint32_t, 4>;

// Whether the compiled branch promises results that are bit-for-bit identical
// to the scalar fallback for NaN and signed-zero inputs.
//
// The scalar fallback trivially does, and the wasm128 branch is written to.
// NEON's fmin/fmax return the NaN operand regardless of which side it is on,
// where the scalar `a < b ? a : b` returns b, and both the NEON and SSE2
// clamps are min/max compositions rather than the scalar nested selection, so
// they differ on NaN and on a negative zero input. Those divergences predate
// the wasm branch, so the strict bitwise expectations below are scoped to the
// branches that guarantee them.
#if defined(TINY_SKIA_SIMD_NEON) || defined(TINY_SKIA_SIMD_SSE2)
constexpr bool kBitwiseScalarParity = false;
#else
constexpr bool kBitwiseScalarParity = true;
#endif

/// Scalar reference for the uint8 accumulator: per-channel sums, divided with
/// the same ScaledDivider and truncated the way storeToU8 truncates.
Bytes referenceBoxAverage(const std::array<Bytes, 8>& pixels, int count,
                          const ScaledDivider& divider) {
  Bytes out{};
  for (std::size_t channel = 0; channel < 4; ++channel) {
    std::uint32_t sum = 0;
    for (int i = 0; i < count; ++i) {
      sum += pixels[static_cast<std::size_t>(i)][channel];
    }
    out[channel] = static_cast<std::uint8_t>(divider.divide(sum));
  }

  return out;
}

// The byte load/store helpers move four bytes as one 32-bit unit, so every
// buffer handed to them is kept 32-bit aligned.
Bytes storeBytes(const Vec4u32& value) {
  alignas(4) Bytes out{};
  value.storeToU8(out.data());
  return out;
}

Bytes storeBytes(const Vec4u8& value) {
  alignas(4) Bytes out{};
  value.store(out.data());
  return out;
}

Floats storeFloats(const Vec4f32& value) {
  Floats out{};
  value.store(out.data());
  return out;
}

/// Bit patterns of each lane, so NaN payloads and zero signs are observable.
/// Comparing floats with `==` reports -0.0 and +0.0 as equal and every NaN as
/// unequal, neither of which can express a bit-for-bit expectation.
FloatBits bitsOf(const Floats& values) {
  FloatBits out{};
  for (std::size_t lane = 0; lane < 4; ++lane) {
    out[lane] = std::bit_cast<std::uint32_t>(values[lane]);
  }

  return out;
}

FloatBits storeBits(const Vec4f32& value) { return bitsOf(storeFloats(value)); }

const std::array<Bytes, 8>& samplePixels() {
  alignas(4) static const std::array<Bytes, 8> kPixels = {{{{0, 0, 0, 0}},
                                                           {{255, 255, 255, 255}},
                                                           {{1, 2, 3, 4}},
                                                           {{200, 17, 99, 128}},
                                                           {{255, 0, 128, 1}},
                                                           {{7, 250, 64, 192}},
                                                           {{33, 34, 35, 36}},
                                                           {{129, 130, 131, 132}}}};
  return kPixels;
}

// ---------------------------------------------------------------------------
// ScaledDivider
// ---------------------------------------------------------------------------

TEST(ScaledDividerTest, MatchesRoundedIntegerDivision) {
  // Divisor 1 is outside the type's domain: its reciprocal is 2^32 and does
  // not fit the 32-bit factor. Blur passes with a one-sample window are
  // skipped before a divider is built.
  for (std::uint32_t divisor = 2; divisor <= 256; ++divisor) {
    const ScaledDivider divider(divisor);
    // The blur accumulator never exceeds 255 samples of 255, which is the
    // range this reciprocal approximation is dimensioned for.
    const std::uint32_t maxNum = 255u * divisor;
    for (std::uint32_t num = 0; num <= maxNum; num += (divisor * 7u) + 1u) {
      EXPECT_EQ(divider.divide(num), (num + divisor / 2) / divisor)
          << "divisor=" << divisor << " num=" << num;
    }
    EXPECT_EQ(divider.divide(maxNum), (maxNum + divisor / 2) / divisor) << "divisor=" << divisor;
  }
}

// ---------------------------------------------------------------------------
// Vec4u32
// ---------------------------------------------------------------------------

TEST(Vec4u32Test, DefaultIsZero) {
  EXPECT_THAT(storeBytes(Vec4u32()), ElementsAreArray((Bytes{0, 0, 0, 0})));
}

TEST(Vec4u32Test, SplatFillsEveryLane) {
  for (std::uint32_t value = 0; value <= 255; ++value) {
    const auto expected = static_cast<std::uint8_t>(value);
    EXPECT_THAT(storeBytes(Vec4u32::splat(value)),
                ElementsAreArray((Bytes{expected, expected, expected, expected})));
  }
}

TEST(Vec4u32Test, LoadFromU8StoreToU8RoundTrips) {
  for (const Bytes& pixel : samplePixels()) {
    EXPECT_THAT(storeBytes(Vec4u32::loadFromU8(pixel.data())), ElementsAreArray(pixel));
  }

  // Sweep every byte value through one lane at a time.
  for (int value = 0; value <= 255; ++value) {
    for (std::size_t lane = 0; lane < 4; ++lane) {
      alignas(4) Bytes pixel{9, 9, 9, 9};
      pixel[lane] = static_cast<std::uint8_t>(value);
      EXPECT_THAT(storeBytes(Vec4u32::loadFromU8(pixel.data())), ElementsAreArray(pixel));
    }
  }
}

TEST(Vec4u32Test, AddAndSubtractAreLanewiseInverses) {
  // The running box blur adds an entering sample and subtracts a leaving one,
  // so the accumulator only has to round-trip. Values are observed after
  // narrowing back to bytes, which is only defined for in-range lanes; the
  // add-then-subtract shape keeps the observed values in range while still
  // failing on any lane mixing inside the vector operations.
  const auto& pixels = samplePixels();
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    for (std::size_t j = 0; j < pixels.size(); ++j) {
      const Vec4u32 a = Vec4u32::loadFromU8(pixels[i].data());
      const Vec4u32 b = Vec4u32::loadFromU8(pixels[j].data());

      EXPECT_THAT(storeBytes((a + b) - b), ElementsAreArray(pixels[i])) << "i=" << i << " j=" << j;
      EXPECT_THAT(storeBytes((a - b) + b), ElementsAreArray(pixels[i])) << "i=" << i << " j=" << j;

      Vec4u32 accumulator = a;
      accumulator += b;
      accumulator -= b;
      EXPECT_THAT(storeBytes(accumulator), ElementsAreArray(pixels[i])) << "i=" << i << " j=" << j;
    }
  }
}

TEST(Vec4u32Test, AddAndSubtractMatchPerLaneArithmeticInByteRange) {
  alignas(4) static const std::array<Bytes, 4> kSmall = {
      {{{0, 1, 2, 3}}, {{10, 20, 30, 40}}, {{100, 50, 25, 12}}, {{127, 100, 3, 0}}}};

  for (const Bytes& lhsBytes : kSmall) {
    for (const Bytes& rhsBytes : kSmall) {
      // expectedSum is loaded back through loadFromU8 below, so it has to
      // satisfy the same 32-bit alignment invariant as every other buffer
      // handed to the byte load/store helpers.
      alignas(4) Bytes expectedSum{};
      for (std::size_t lane = 0; lane < 4; ++lane) {
        expectedSum[lane] = static_cast<std::uint8_t>(std::uint32_t{lhsBytes[lane]} +
                                                      std::uint32_t{rhsBytes[lane]});
      }

      const Vec4u32 lhs = Vec4u32::loadFromU8(lhsBytes.data());
      const Vec4u32 rhs = Vec4u32::loadFromU8(rhsBytes.data());
      EXPECT_THAT(storeBytes(lhs + rhs), ElementsAreArray(expectedSum));

      Vec4u32 accumulator = lhs;
      accumulator += rhs;
      EXPECT_THAT(storeBytes(accumulator), ElementsAreArray(expectedSum));
      EXPECT_THAT(storeBytes(Vec4u32::loadFromU8(expectedSum.data()) - rhs),
                  ElementsAreArray(lhsBytes));
    }
  }
}

TEST(Vec4u32Test, ScaledDivideMatchesScalarDivider) {
  const auto& pixels = samplePixels();
  for (int count = 2; count <= static_cast<int>(pixels.size()); ++count) {
    const ScaledDivider divider(static_cast<std::uint32_t>(count));

    Vec4u32 sum;
    for (int i = 0; i < count; ++i) {
      sum += Vec4u32::loadFromU8(pixels[static_cast<std::size_t>(i)].data());
    }

    EXPECT_THAT(storeBytes(sum.scaledDivide(divider)),
                ElementsAreArray(referenceBoxAverage(pixels, count, divider)))
        << "count=" << count;
  }
}

TEST(Vec4u32Test, ScaledDivideCoversWideDivisors) {
  // Exercise the widening multiply across accumulator magnitudes a running box
  // blur actually produces, including the largest supported kernel.
  for (std::uint32_t divisor : {2u, 3u, 5u, 17u, 64u, 129u, 255u}) {
    const ScaledDivider divider(divisor);
    for (std::uint32_t base : {0u, 1u, 127u, 128u, 254u, 255u}) {
      const Vec4u32 sum = Vec4u32::splat(base * divisor);
      const auto expected = static_cast<std::uint8_t>(divider.divide(base * divisor));
      EXPECT_THAT(storeBytes(sum.scaledDivide(divider)),
                  ElementsAreArray((Bytes{expected, expected, expected, expected})))
          << "divisor=" << divisor << " base=" << base;
    }
  }
}

// ---------------------------------------------------------------------------
// Vec4f32
// ---------------------------------------------------------------------------

/// Ordinary finite samples, deliberately free of NaN and of any lane that
/// pairs +0.0 against -0.0.
///
/// The tests built on these compare float values, and every branch agrees on
/// them. NaN and signed-zero selection is where the branches legitimately
/// differ (see kBitwiseScalarParity), so it is covered separately and
/// bitwise by NanAndSignedZeroSelectionMatchesScalarBitwise rather than being
/// mixed into the general samples, where it would assert a divergence that
/// NEON and SSE2 are not expected to satisfy.
const std::array<Floats, 6>& sampleFloats() {
  static const std::array<Floats, 6> kValues = {{{{0.0f, 1.0f, 0.5f, 0.25f}},
                                                 {{-1.0f, 2.0f, -0.5f, 0.75f}},
                                                 {{1.5f, 1.5f, 0.0f, -0.0f}},
                                                 {{3.25f, -4.5f, 0.125f, 1.0f}},
                                                 {{-2.75f, 0.0f, 8.0f, -16.0f}},
                                                 {{0.0625f, 0.375f, 0.9375f, 2.5f}}}};
  return kValues;
}

TEST(Vec4f32Test, DefaultIsZero) {
  EXPECT_THAT(storeFloats(Vec4f32()), ElementsAreArray((Floats{0.0f, 0.0f, 0.0f, 0.0f})));
}

TEST(Vec4f32Test, LoadStoreRoundTripsAndSplatFillsEveryLane) {
  for (const Floats& values : sampleFloats()) {
    EXPECT_THAT(storeFloats(Vec4f32::load(values.data())), ElementsAreArray(values));
    for (const float lane : values) {
      EXPECT_THAT(storeFloats(Vec4f32::splat(lane)),
                  ElementsAreArray((Floats{lane, lane, lane, lane})));
    }
  }
}

TEST(Vec4f32Test, ArithmeticMatchesPerLaneOperations) {
  const auto& values = sampleFloats();
  for (const Floats& lhsValues : values) {
    for (const Floats& rhsValues : values) {
      const Vec4f32 lhs = Vec4f32::load(lhsValues.data());
      const Vec4f32 rhs = Vec4f32::load(rhsValues.data());

      Floats expectedAdd{};
      Floats expectedSub{};
      Floats expectedMul{};
      for (std::size_t lane = 0; lane < 4; ++lane) {
        expectedAdd[lane] = lhsValues[lane] + rhsValues[lane];
        expectedSub[lane] = lhsValues[lane] - rhsValues[lane];
        expectedMul[lane] = lhsValues[lane] * rhsValues[lane];
      }

      EXPECT_THAT(storeFloats(lhs + rhs), ElementsAreArray(expectedAdd));
      EXPECT_THAT(storeFloats(lhs - rhs), ElementsAreArray(expectedSub));
      EXPECT_THAT(storeFloats(lhs * rhs), ElementsAreArray(expectedMul));

      Vec4f32 accumulate = lhs;
      accumulate += rhs;
      EXPECT_THAT(storeFloats(accumulate), ElementsAreArray(expectedAdd));

      Vec4f32 decumulate = lhs;
      decumulate -= rhs;
      EXPECT_THAT(storeFloats(decumulate), ElementsAreArray(expectedSub));
    }
  }
}

TEST(Vec4f32Test, MinAndMaxMatchScalarSelection) {
  const auto& values = sampleFloats();
  for (const Floats& lhsValues : values) {
    for (const Floats& rhsValues : values) {
      const Vec4f32 lhs = Vec4f32::load(lhsValues.data());
      const Vec4f32 rhs = Vec4f32::load(rhsValues.data());

      Floats expectedMin{};
      Floats expectedMax{};
      for (std::size_t lane = 0; lane < 4; ++lane) {
        expectedMin[lane] = lhsValues[lane] < rhsValues[lane] ? lhsValues[lane] : rhsValues[lane];
        expectedMax[lane] = lhsValues[lane] > rhsValues[lane] ? lhsValues[lane] : rhsValues[lane];
      }

      EXPECT_THAT(storeFloats(Vec4f32::min(lhs, rhs)), ElementsAreArray(expectedMin));
      EXPECT_THAT(storeFloats(Vec4f32::max(lhs, rhs)), ElementsAreArray(expectedMax));
    }
  }
}

TEST(Vec4f32Test, Clamp01MatchesScalarClamp) {
  for (const Floats& values : sampleFloats()) {
    Floats expected{};
    for (std::size_t lane = 0; lane < 4; ++lane) {
      expected[lane] = values[lane] < 0.0f ? 0.0f : (values[lane] > 1.0f ? 1.0f : values[lane]);
    }

    EXPECT_THAT(storeFloats(Vec4f32::load(values.data()).clamp01()), ElementsAreArray(expected));
  }
}

TEST(Vec4f32Test, ClampMaxMatchesScalarClamp) {
  // Limits are non-negative: clampMax is only defined for an upper bound at or
  // above the lower bound of zero, and callers pass channel maxima.
  static const std::array<Floats, 3> kLimits = {
      {{{1.0f, 1.0f, 1.0f, 1.0f}}, {{0.5f, 2.0f, 0.0f, 4.0f}}, {{8.0f, 0.25f, 3.5f, 0.0f}}}};

  const auto& values = sampleFloats();
  for (const Floats& source : values) {
    for (const Floats& limit : kLimits) {
      Floats expected{};
      for (std::size_t lane = 0; lane < 4; ++lane) {
        expected[lane] =
            source[lane] < 0.0f ? 0.0f : (source[lane] > limit[lane] ? limit[lane] : source[lane]);
      }

      EXPECT_THAT(storeFloats(Vec4f32::load(source.data()).clampMax(Vec4f32::load(limit.data()))),
                  ElementsAreArray(expected));
    }
  }
}

TEST(Vec4f32Test, FmaddMatchesMultiplyAdd) {
  // Branches differ in whether the multiply and add are fused into a single
  // rounding step, so this uses operands whose products and sums are exactly
  // representable and therefore identical under both.
  const std::array<Floats, 4> kExact = {{{{0.0f, 1.0f, 2.0f, 3.0f}},
                                         {{4.0f, -2.0f, 0.5f, 0.25f}},
                                         {{-8.0f, 16.0f, 1.5f, -0.125f}},
                                         {{32.0f, 0.0f, -4.0f, 6.0f}}}};
  for (const Floats& aValues : kExact) {
    for (const Floats& bValues : kExact) {
      for (const Floats& cValues : kExact) {
        Floats expected{};
        for (std::size_t lane = 0; lane < 4; ++lane) {
          const float product = aValues[lane] * bValues[lane];
          expected[lane] = product + cValues[lane];
        }

        EXPECT_THAT(
            storeFloats(Vec4f32::fmadd(Vec4f32::load(aValues.data()), Vec4f32::load(bValues.data()),
                                       Vec4f32::load(cValues.data()))),
            ElementsAreArray(expected));
      }
    }
  }
}

TEST(Vec4f32Test, NanAndSignedZeroSelectionMatchesScalarBitwise) {
  if (!kBitwiseScalarParity) {
    GTEST_SKIP() << "branch has a documented pre-existing NaN and signed-zero divergence";
  }

  // Every lane here is a case where the scalar comparison is false, so the
  // result is the operand the scalar formula names in its else position. A
  // branch that selects from the other operand (a min/max with its arguments
  // the wrong way round) produces the other value and fails.
  const float nan = std::bit_cast<float>(std::uint32_t{0x7fc00000});
  const std::array<Floats, 2> kLhs = {{{{nan, 1.0f, -0.0f, 3.0f}}, {{0.0f, -0.0f, nan, -1.0f}}}};
  const std::array<Floats, 2> kRhs = {{{{1.0f, nan, 0.0f, 2.0f}}, {{-0.0f, 0.0f, nan, nan}}}};

  for (std::size_t i = 0; i < kLhs.size(); ++i) {
    const Vec4f32 lhs = Vec4f32::load(kLhs[i].data());
    const Vec4f32 rhs = Vec4f32::load(kRhs[i].data());

    Floats expectedMin{};
    Floats expectedMax{};
    for (std::size_t lane = 0; lane < 4; ++lane) {
      expectedMin[lane] = kLhs[i][lane] < kRhs[i][lane] ? kLhs[i][lane] : kRhs[i][lane];
      expectedMax[lane] = kLhs[i][lane] > kRhs[i][lane] ? kLhs[i][lane] : kRhs[i][lane];
    }

    EXPECT_THAT(storeBits(Vec4f32::min(lhs, rhs)), ElementsAreArray(bitsOf(expectedMin)))
        << "case=" << i;
    EXPECT_THAT(storeBits(Vec4f32::max(lhs, rhs)), ElementsAreArray(bitsOf(expectedMax)))
        << "case=" << i;
  }

  // clamp01 and clampMax are a nested selection, not a min/max composition:
  // a NaN lane falls through both comparisons unchanged, and a negative zero
  // input is neither below zero nor above the limit, so it survives.
  const Floats kSource = {nan, -0.0f, 2.0f, -1.0f};
  const Floats kLimit = {1.0f, 1.0f, 1.5f, 0.0f};

  Floats expectedClamp01{};
  Floats expectedClampMax{};
  for (std::size_t lane = 0; lane < 4; ++lane) {
    expectedClamp01[lane] =
        kSource[lane] < 0.0f ? 0.0f : (kSource[lane] > 1.0f ? 1.0f : kSource[lane]);
    expectedClampMax[lane] =
        kSource[lane] < 0.0f ? 0.0f : (kSource[lane] > kLimit[lane] ? kLimit[lane] : kSource[lane]);
  }

  const Vec4f32 source = Vec4f32::load(kSource.data());
  EXPECT_THAT(storeBits(source.clamp01()), ElementsAreArray(bitsOf(expectedClamp01)));
  EXPECT_THAT(storeBits(source.clampMax(Vec4f32::load(kLimit.data()))),
              ElementsAreArray(bitsOf(expectedClampMax)));
}

// ---------------------------------------------------------------------------
// Vec4u8
// ---------------------------------------------------------------------------

TEST(Vec4u8Test, DefaultIsZero) {
  EXPECT_THAT(storeBytes(Vec4u8()), ElementsAreArray((Bytes{0, 0, 0, 0})));
}

TEST(Vec4u8Test, LoadStoreRoundTrips) {
  for (const Bytes& pixel : samplePixels()) {
    EXPECT_THAT(storeBytes(Vec4u8::load(pixel.data())), ElementsAreArray(pixel));
  }

  for (int value = 0; value <= 255; ++value) {
    for (std::size_t lane = 0; lane < 4; ++lane) {
      alignas(4) Bytes pixel{9, 9, 9, 9};
      pixel[lane] = static_cast<std::uint8_t>(value);
      EXPECT_THAT(storeBytes(Vec4u8::load(pixel.data())), ElementsAreArray(pixel));
    }
  }
}

TEST(Vec4u8Test, MinAndMaxMatchScalarSelection) {
  const auto& pixels = samplePixels();
  for (const Bytes& lhsBytes : pixels) {
    for (const Bytes& rhsBytes : pixels) {
      Bytes expectedMin{};
      Bytes expectedMax{};
      for (std::size_t lane = 0; lane < 4; ++lane) {
        expectedMin[lane] = lhsBytes[lane] < rhsBytes[lane] ? lhsBytes[lane] : rhsBytes[lane];
        expectedMax[lane] = lhsBytes[lane] > rhsBytes[lane] ? lhsBytes[lane] : rhsBytes[lane];
      }

      const Vec4u8 lhs = Vec4u8::load(lhsBytes.data());
      const Vec4u8 rhs = Vec4u8::load(rhsBytes.data());
      EXPECT_THAT(storeBytes(Vec4u8::min(lhs, rhs)), ElementsAreArray(expectedMin));
      EXPECT_THAT(storeBytes(Vec4u8::max(lhs, rhs)), ElementsAreArray(expectedMax));
    }
  }
}

TEST(Vec4u8Test, MinAndMaxCoverTheFullByteRange) {
  for (int lhsValue = 0; lhsValue <= 255; ++lhsValue) {
    const int rhsValue = 255 - lhsValue;
    alignas(4) const Bytes lhsBytes{static_cast<std::uint8_t>(lhsValue), 0, 255,
                                    static_cast<std::uint8_t>(lhsValue)};
    alignas(4) const Bytes rhsBytes{static_cast<std::uint8_t>(rhsValue), 255, 0,
                                    static_cast<std::uint8_t>(lhsValue)};

    Bytes expectedMin{};
    Bytes expectedMax{};
    for (std::size_t lane = 0; lane < 4; ++lane) {
      expectedMin[lane] = lhsBytes[lane] < rhsBytes[lane] ? lhsBytes[lane] : rhsBytes[lane];
      expectedMax[lane] = lhsBytes[lane] > rhsBytes[lane] ? lhsBytes[lane] : rhsBytes[lane];
    }

    const Vec4u8 lhs = Vec4u8::load(lhsBytes.data());
    const Vec4u8 rhs = Vec4u8::load(rhsBytes.data());
    EXPECT_THAT(storeBytes(Vec4u8::min(lhs, rhs)), ElementsAreArray(expectedMin))
        << "lhs=" << lhsValue;
    EXPECT_THAT(storeBytes(Vec4u8::max(lhs, rhs)), ElementsAreArray(expectedMax))
        << "lhs=" << lhsValue;
  }
}

}  // namespace
}  // namespace tiny_skia::filter
