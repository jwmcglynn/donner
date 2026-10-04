#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/tests/ProjectionTestSupport.h"

namespace donner::gpu::shader::wgsl {
namespace {

using testing::Contains;
using testing::Eq;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;
using tests::ComputeModule;
using tests::ConstantNamed;
using tests::ConstantValue;
using tests::Diagnose;
using tests::kAccepted;
using tests::Msl;
using tests::Opcodes;
using tests::Rejection;
using tests::Spirv;
using tests::Wgsl;

// SPIR-V opcode of a floating-point division.
constexpr uint32_t kOpFDiv = 136;

/// Returns the folded f32 constant with bits p bits.
ConstantValue F32Bits(uint32_t bits) {
  return ConstantValue{Type{TypeKind::F32}, int64_t(bits)};
}

TEST(FloatConstant, FoldsF32ArithmeticToTheCorrectlyRoundedValue) {
  const std::string source = R"(
const kThird: f32 = 1f / 3f;
const kSum = 0.1f + 0.2f;
const kProduct = 2f * 1.5f;
const kChain = 1f / 3f * 3f;
const kPerOperation = 16777216f + 1f - 16777216f;
const kSubnormal = 1.17549435e-38f / 2f;
const kFromAbstractFloat = 1.0 / 3f;
const kFromAbstractInt = 2 * 1.5f;
)";
  const ParseResult parsed = Parse(source);
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));
  EXPECT_THAT(ConstantNamed(parsed.module, "kThird"), Eq(F32Bits(0x3eaaaaab)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kSum"), Eq(F32Bits(0x3e99999a)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kProduct"), Eq(F32Bits(0x40400000)));
  // Each operation rounds to f32, so the third times three is exactly one.
  EXPECT_THAT(ConstantNamed(parsed.module, "kChain"), Eq(F32Bits(0x3f800000)));
  // 2^24 + 1 rounds to 2^24 in f32 before the subtraction, unlike abstract arithmetic.
  EXPECT_THAT(ConstantNamed(parsed.module, "kPerOperation"), Eq(F32Bits(0)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kSubnormal"), Eq(F32Bits(0x00400000)));
  // An abstract operand converts to f32 first, then the operation rounds in f32.
  EXPECT_THAT(ConstantNamed(parsed.module, "kFromAbstractFloat"), Eq(F32Bits(0x3eaaaaab)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kFromAbstractInt"), Eq(F32Bits(0x40400000)));
}

TEST(FloatConstant, FoldsAgainThroughAScalarConstant) {
  // A folded scalar module constant is a literal wherever it is referenced.
  const std::string source = R"(
const kThird: f32 = 1f / 3f;
const kReused = kThird * 3f;
fn f(x: f32) -> f32 { return x * (kThird * 3f); }
)";
  const ParseResult parsed = Parse(source);
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));
  EXPECT_THAT(ConstantNamed(parsed.module, "kReused"), Eq(F32Bits(0x3f800000)));

  // A module constant must be a literal, so a vector-valued one is rejected at its declaration
  // before it can be a folding operand.
  for (const auto& [vectorSource, expected] : std::vector<std::pair<std::string, Rejection>>{
           {"const kPair = vec2<f32>(1f, 2f);\nfn f() -> vec2<f32> { return kPair * 2f; }",
            {ErrorCode::UnsupportedConstruct, "kPair"}},
           {"const kHalves = vec2<f32>(1f, 2f) / 2f;",
            {ErrorCode::UnsupportedConstruct, "kHalves"}},
       }) {
    SCOPED_TRACE(vectorSource);
    EXPECT_THAT(Diagnose(vectorSource), Eq(expected));
  }
}

TEST(FloatConstant, SignsZeroResultsAsIeeeRoundToNearest) {
  const std::string source = R"(
const kCancelled = 1f - 1f;
const kNegativeCancelled = -1f + 1f;
const kNegativeMinusNegative = -1f - -1f;
const kZeroSum = -0f + 0f;
const kNegativeZeroDifference = -0f - 0f;
const kNegativeZeroSum = -0f + -0f;
const kNegativeProduct = 0f * -1f;
const kAbstractCancelled = -1.0 + 1.0;
)";
  const ParseResult parsed = Parse(source);
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));
  // An exact cancellation is +0 whatever the operand signs; only -0 - +0 and -0 + -0 stay -0.
  EXPECT_THAT(ConstantNamed(parsed.module, "kCancelled"), Eq(F32Bits(0)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kNegativeCancelled"), Eq(F32Bits(0)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kNegativeMinusNegative"), Eq(F32Bits(0)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kZeroSum"), Eq(F32Bits(0)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kNegativeZeroDifference"), Eq(F32Bits(0x80000000)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kNegativeZeroSum"), Eq(F32Bits(0x80000000)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kNegativeProduct"), Eq(F32Bits(0x80000000)));
  EXPECT_THAT(ConstantNamed(parsed.module, "kAbstractCancelled"),
              Eq(ConstantValue{Type{TypeKind::AbstractFloat}, 0}));
}

TEST(FloatConstant, RejectsResultsThatAreNotFinite) {
  for (const auto& [source, expected] : std::vector<std::pair<std::string, Rejection>>{
           {"const k = 1f / 0f;", {ErrorCode::InvalidConstantExpression, "/"}},
           {"const k = -1f / 0f;", {ErrorCode::InvalidConstantExpression, "/"}},
           {"const k = 0f / 0f;", {ErrorCode::InvalidConstantExpression, "/"}},
           {"const k = 1f / -0f;", {ErrorCode::InvalidConstantExpression, "/"}},
           {"const k = 3.402823466e38f * 10f;", {ErrorCode::InvalidConstantExpression, "*"}},
           {"const k = -3.402823466e38f - 3.402823466e38f;",
            {ErrorCode::InvalidConstantExpression, "-"}},
           // Underflow to a subnormal or to zero is finite and folds.
           {"const k = 1e-30f / 1e10f / 1e10f / 1e30f;", {ErrorCode::None, ""}},
           // WGSL lets a result between the largest finite f32 and 2^128 round either way; the
           // profile rounds it to infinity, which is a shader-creation error.
           {"const k = 3.402823466e38f + 1f;", {ErrorCode::InvalidConstantExpression, "+"}},
           {"fn f() -> vec2<f32> { return vec2<f32>(1f, 2f) / vec2<f32>(1f, 0f); }",
            {ErrorCode::InvalidConstantExpression, "/"}},
           {"fn f() -> vec2<f32> { return vec2<f32>(3.402823466e38f, 1f) * 2f; }",
            {ErrorCode::InvalidConstantExpression, "*"}},
       }) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(expected));
  }
}

TEST(FloatConstant, KeepsOperatorsAndOperandsOutsideTheFoldRejected) {
  for (const auto& [source, expected] : std::vector<std::pair<std::string, Rejection>>{
           // WGSL defines f32 remainder, but the profile has no f32 `%` at all.
           {"fn f() -> f32 { return 5f % 3f; }", {ErrorCode::TypeMismatch, "%"}},
           {"const k = 5.0 % 3.0;", {ErrorCode::TypeMismatch, "%"}},
           // Only literals, their negations and vector constructions of them fold.
           {"fn f() -> f32 { return vec2<f32>(1f, 2f).x / 2f; }",
            {ErrorCode::InvalidConstantExpression, "/"}},
           {"struct S { x: f32, }\nfn f() -> f32 { return S(1f).x * 2f; }",
            {ErrorCode::InvalidConstantExpression, "*"}},
           {"fn f() -> f32 { return f32(1f) * 2f; }", {ErrorCode::InvalidConstantExpression, "*"}},
       }) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(expected));
  }
  for (const char* source : {
           "fn f(x: f32) -> f32 { return x / 3f; }",
           "fn f(x: f32) -> f32 { let third = 1f / 3f; return x * third; }",
           "fn f(x: vec2<f32>) -> vec2<f32> { return x * (vec2<f32>(1f, 2f) / 3f); }",
           "fn f(x: f32) -> f32 { return x * (-1f / -3f); }",
       }) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(kAccepted));
  }
}

TEST(FloatConstant, ProjectsFoldedValuesToEveryTarget) {
  const std::string source = ComputeModule(
      "fn third() -> f32 { return 1f / 3f; }\n"
      "fn thirds() -> vec2<f32> { return vec2<f32>(1f, 2f) / 3f; }\n"
      "fn doubled() -> vec2<f32> { return 2f * vec2<f32>(1f, 3f); }",
      "third() + thirds().y + doubled().y");
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));

  // The WGSL projection carries the folded values too, so a browser evaluates nothing.
  const std::string wgsl = Wgsl(source);
  ASSERT_THAT(wgsl, Not(IsEmpty()));
  EXPECT_THAT(wgsl, HasSubstr("return 0x1.555556p-2f;"));
  EXPECT_THAT(wgsl, HasSubstr("return vec2<f32>(0x1.555556p-2f, 0x1.555556p-1f);"));
  EXPECT_THAT(wgsl, Not(HasSubstr("/ 3f")));
  EXPECT_THAT(Msl(wgsl), Eq(Msl(source)));
  EXPECT_THAT(Spirv(wgsl), Eq(Spirv(source)));

  // The native projections carry the folded values and no division.
  const std::string msl = Msl(source);
  ASSERT_THAT(msl, Not(IsEmpty()));
  EXPECT_THAT(msl, HasSubstr("0x1.555556p-2f"));
  EXPECT_THAT(msl, HasSubstr("0x1.555556p-1f"));
  EXPECT_THAT(msl, HasSubstr("0x1.800000p2f"));
  EXPECT_THAT(msl, Not(HasSubstr(" / ")));

  const std::vector<uint32_t> words = Spirv(source);
  ASSERT_THAT(words, Not(IsEmpty()));
  EXPECT_THAT(words, Contains(0x3eaaaaabu));
  EXPECT_THAT(words, Contains(0x3f2aaaabu));
  EXPECT_THAT(words, Contains(0x40c00000u));
  EXPECT_THAT(Opcodes(words), Not(Contains(kOpFDiv)));
}

TEST(FloatConstant, ReplacesEachOutermostFoldWithItsExactValueInTheWgslProjection) {
  // WGSL lets a browser compute f32 division to within 2.5 ULP and convert an inexact literal in
  // either direction, so the projection spells every folded value exactly: positive values as hex
  // floats, and negative values as an exact subtraction from -0f that this compiler folds back.
  const std::string source = ComputeModule(
      "const kThird: f32 = 1f / 3f;\n"
      "fn weight() -> f32 { let b = 1f / 3f; return b; }\n"
      "fn negative(x: f32) -> f32 { return x-1f*-3f; }\n"
      "fn thirds() -> vec2<f32> { return vec2<f32>(1f, 2f) / 3f; }\n"
      "fn signs() -> vec2<f32> { return vec2<f32>(1f, -2f) * -1f; }\n"
      "fn tiny() -> f32 { return 1.17549435e-38f / 2f; }\n"
      "fn negativeZero() -> f32 { return -0f - 0f; }\n"
      "fn nested() -> f32 { return (1f / 3f) * (3f); }\n"
      "fn spread() -> f32 {\n"
      "  return 1f /  // the divisor follows\n"
      "    4f;\n"
      "}\n"
      "fn kw() -> f32 { return-1f*-3f; }\n"
      "fn kp() -> f32 { return(1f)/3f; }\n"
      "fn kv() -> vec2<f32> { return(vec2<f32>(1f, 2f))*2f; }\n",
      "weight() + negative(1f) + thirds().y + signs().x + tiny() + negativeZero() + nested() + "
      "spread() + kThird + kw() + kp() + kv().y");
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));
  const std::string wgsl = Wgsl(source);
  ASSERT_THAT(wgsl, Not(IsEmpty()));
  for (const char* line : {
           "const kThird: f32 = 0x1.555556p-2f;\n",
           "fn weight() -> f32 { let b = 0x1.555556p-2f; return b; }\n",
           "fn negative(x: f32) -> f32 { return x-(-0f - 0x1.800000p1f); }\n",
           "fn thirds() -> vec2<f32> { return vec2<f32>(0x1.555556p-2f, 0x1.555556p-1f); }\n",
           "fn signs() -> vec2<f32> { return vec2<f32>((-0f - 0x1.000000p0f), 0x1.000000p1f); }\n",
           "fn tiny() -> f32 { return 0x1.000000p-127f; }\n",
           "fn negativeZero() -> f32 { return (-0f - 0.0f); }\n",
           "fn nested() -> f32 { return 0x1.000000p0f; }\n",
           "return 0x1.000000p-2f;\n",
           // A value is separated from an adjacent keyword, so the splice never joins tokens.
           "fn kw() -> f32 { return 0x1.800000p1f; }\n",
           "fn kp() -> f32 { return 0x1.555556p-2f; }\n",
           "fn kv() -> vec2<f32> { return vec2<f32>(0x1.000000p1f, 0x1.000000p2f); }\n",
       }) {
    EXPECT_THAT(wgsl, HasSubstr(line));
  }
  EXPECT_THAT(wgsl, Not(HasSubstr("--")));
  EXPECT_THAT(wgsl, Not(HasSubstr("divisor")));
  EXPECT_THAT(wgsl, Not(HasSubstr("4f;")));

  // The projection is itself valid input that folds to the same native bytes and projects to
  // itself, so a shipped projection is canonical.
  ASSERT_THAT(Diagnose(wgsl), Eq(kAccepted));
  EXPECT_THAT(Wgsl(wgsl), Eq(wgsl));
  ASSERT_THAT(Msl(source), Not(IsEmpty()));
  EXPECT_THAT(Msl(wgsl), Eq(Msl(source)));
  ASSERT_THAT(Spirv(source), Not(IsEmpty()));
  EXPECT_THAT(Spirv(wgsl), Eq(Spirv(source)));
  EXPECT_THAT(ConstantNamed(Parse(wgsl).module, "kThird"), Eq(F32Bits(0x3eaaaaab)));
}

TEST(FloatConstant, ReplacesAbstractFloatFoldsWithTheirExactValuesInTheWgslProjection) {
  // WGSL leaves abstract-float division unbounded where f32 division is not correctly rounded, so
  // an abstract fold is spelled exactly too: as f32 once the parser has given it an f32 type, and
  // as an abstract hex float, negated in parentheses when negative, while it stays abstract.
  const std::string source = ComputeModule(
      "const kAbstractThird = 1.0 / 3.0;\n"
      "const kAbstractNegative = 1.0 - 3.0;\n"
      "fn fraction(x: f32) -> f32 { return max(x, 1.0 / 65536.0); }\n"
      "fn tenth() -> f32 { return 1.0 / 10.0 * 1f; }\n"
      "fn ordered() -> bool { return 1.0 / 3.0 < 0.5; }\n"
      "const kNegatedAbstract = -(1.0 / 3.0);\n"
      "fn negatedThird() -> f32 { let b = -(1.0 / 3.0); return b; }\n",
      "fraction(1f) + tenth() + select(0f, 1f, ordered()) + kAbstractThird + kAbstractNegative + "
      "kNegatedAbstract + negatedThird()");
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));
  const std::string wgsl = Wgsl(source);
  ASSERT_THAT(wgsl, Not(IsEmpty()));
  for (const char* line : {
           "const kAbstractThird = 0x1.5555555555555p-2;\n",
           "const kAbstractNegative = (-0x1.0000000000000p1);\n",
           "fn fraction(x: f32) -> f32 { return max(x, 0x1.000000p-16f); }\n",
           "fn tenth() -> f32 { return 0x1.99999ap-4f; }\n",
           "fn ordered() -> bool { return 0x1.5555555555555p-2 < 0.5; }\n",
           // A negation applied to a fold is part of the outermost fold, so the browser converts
           // nothing.
           "const kNegatedAbstract = (-0x1.5555555555555p-2);\n",
           "fn negatedThird() -> f32 { let b = (-0f - 0x1.555556p-2f); return b; }\n",
       }) {
    EXPECT_THAT(wgsl, HasSubstr(line));
  }
  ASSERT_THAT(Diagnose(wgsl), Eq(kAccepted));
  EXPECT_THAT(Wgsl(wgsl), Eq(wgsl));
  ASSERT_THAT(Msl(source), Not(IsEmpty()));
  EXPECT_THAT(Msl(wgsl), Eq(Msl(source)));
  ASSERT_THAT(Spirv(source), Not(IsEmpty()));
  EXPECT_THAT(Spirv(wgsl), Eq(Spirv(source)));
  const Type abstractFloat{TypeKind::AbstractFloat};
  EXPECT_THAT(ConstantNamed(Parse(wgsl).module, "kAbstractThird"),
              Eq(ConstantValue{abstractFloat, 0x3fd5555555555555}));
  EXPECT_THAT(ConstantNamed(Parse(wgsl).module, "kAbstractNegative"),
              Eq(ConstantValue{abstractFloat, int64_t(0xc000000000000000)}));
}

TEST(FloatConstant, BoundsTheFoldsTheWgslProjectionReplaces) {
  const auto module = [](int folds) {
    std::string body = "fn f() -> f32 {\n  var sum = 0f;\n";
    for (int i = 0; i < folds; ++i) {
      body += "  sum += 1f / 3f;\n";
    }
    return body + "  return sum;\n}\n";
  };
  EXPECT_THAT(Diagnose(module(256)), Eq(kAccepted));
  const std::string tooMany = module(257);
  const ParseResult parsed = Parse(tooMany);
  EXPECT_THAT(parsed.diagnostic.code, Eq(ErrorCode::ExpressionLimit));
  EXPECT_THAT(parsed.diagnostic.span.begin, Eq(tooMany.rfind("1f / 3f")));
}

TEST(FloatConstant, FoldedExpressionMatchesItsLiteralSpelling) {
  const auto module = [](const char* weight) {
    return ComputeModule(std::string("fn weight() -> f32 { let b = ") + weight + "; return b; }",
                         "weight()");
  };
  const std::string folded = module("1f / 3f");
  const std::string literal = module("0.33333334f");
  ASSERT_THAT(Diagnose(folded), Eq(kAccepted));
  ASSERT_THAT(Msl(folded), Not(IsEmpty()));
  EXPECT_THAT(Msl(folded), Eq(Msl(literal)));
  ASSERT_THAT(Spirv(folded), Not(IsEmpty()));
  EXPECT_THAT(Spirv(folded), Eq(Spirv(literal)));
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
