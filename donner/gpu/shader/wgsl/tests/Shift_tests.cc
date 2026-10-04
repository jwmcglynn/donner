#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/tests/ProjectionTestSupport.h"

namespace donner::gpu::shader::wgsl {
namespace {

using testing::Contains;
using testing::Eq;
using testing::HasSubstr;
using testing::Not;

// SPIR-V opcodes the shift lowering is expected to select.
constexpr uint32_t kOpShiftRightLogical = 194;
constexpr uint32_t kOpShiftRightArithmetic = 195;
constexpr uint32_t kOpShiftLeftLogical = 196;
constexpr uint32_t kOpBitwiseAnd = 199;

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

/// Returns the resolved type of the named declaration, or Void when it is absent.
/// @param module Validated module. @param name Declaration spelling.
Type TypeOfSymbol(const Module& module, std::string_view name) {
  for (uint16_t i = 0; i < module.symbolCount; ++i) {
    if (module.name(module.symbols[i].name) == name) {
      return module.symbols[i].type;
    }
  }
  return Type{};
}

TEST(Shift, AcceptsBothOperatorsOnIntegerScalarsAndVectors) {
  const std::string source = R"(
fn unsignedLeft(value: u32, amount: u32) -> u32 { return value << amount; }
fn unsignedRight(value: u32, amount: u32) -> u32 { return value >> amount; }
fn signedLeft(value: i32, amount: u32) -> i32 { return value << amount; }
fn signedRight(value: i32, amount: u32) -> i32 { return value >> amount; }
fn vectorLeft(value: vec3<u32>, amount: vec3<u32>) -> vec3<u32> { return value << amount; }
fn vectorRight(value: vec2<i32>, amount: vec2u) -> vec2<i32> { return value >> amount; }
fn unpack(packed: u32) -> u32 {
  let green = (packed >> 8u) & 255u;
  let alpha = packed >> 24;
  return green + alpha;
}
fn compound(value: vec2<i32>, amount: u32) -> vec2<i32> {
  var result = value;
  result <<= vec2<u32>(amount, 1u);
  result >>= vec2<u32>(2u);
  var scalar = 1u;
  scalar <<= amount;
  scalar >>= 1;
  return result + vec2<i32>(i32(scalar));
}
fn compare(value: u32, amount: u32, bound: u32) -> bool {
  return value << amount < bound && bound > value >> amount;
}
)";
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));
}

TEST(Shift, TypesTheShiftAmountAsUnsignedAndKeepsTheValueType) {
  const std::string source = R"(
fn widen(amount: u32) -> i32 {
  let fromAbstract = 1 << amount;
  let fromLiteral = 7u >> 1;
  return fromAbstract + i32(fromLiteral);
}
)";
  const ParseResult parsed = Parse(source);
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));
  // An abstract value shifted by a runtime amount concretizes to i32, as WGSL selects the i32
  // overload when the amount is not a constant expression.
  EXPECT_THAT(TypeOfSymbol(parsed.module, "fromAbstract"), Eq(Type{TypeKind::I32}));
  EXPECT_THAT(TypeOfSymbol(parsed.module, "fromLiteral"), Eq(Type{TypeKind::U32}));
}

TEST(Shift, FoldsAbstractConstantsWithSixtyFourBitRules) {
  const std::string source = R"(
const kFlag = 1 << 4;
const kMask = (1 << 8) - 1;
const kHalf = -9 >> 1;
const kMinimum = -1 << 63;
const kUnsignedAmount = 3 << 2u;
)";
  const ParseResult parsed = Parse(source);
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));
  const Type abstractInt{TypeKind::AbstractInt};
  EXPECT_THAT(ConstantNamed(parsed.module, "kFlag"), Eq(ConstantValue{abstractInt, 16}));
  EXPECT_THAT(ConstantNamed(parsed.module, "kMask"), Eq(ConstantValue{abstractInt, 255}));
  // Abstract right shift is arithmetic.
  EXPECT_THAT(ConstantNamed(parsed.module, "kHalf"), Eq(ConstantValue{abstractInt, -5}));
  EXPECT_THAT(ConstantNamed(parsed.module, "kMinimum"), Eq(ConstantValue{abstractInt, INT64_MIN}));
  EXPECT_THAT(ConstantNamed(parsed.module, "kUnsignedAmount"), Eq(ConstantValue{abstractInt, 12}));

  // A module constant holds a literal, and only abstract arithmetic folds into one.
  EXPECT_THAT(Diagnose("const kConcrete = 1u << 4u;"),
              Eq(Rejection{ErrorCode::UnsupportedConstruct, "kConcrete"}));
}

TEST(Shift, EvaluatesConstantShiftsWhereAConstantIsRequired) {
  const std::string source = R"(
struct Table { values: array<f32, (1u << 3u)>, }
fn select_case(x: u32) -> u32 {
  switch (x) {
    case (1u << 2u): { return 1u; }
    case (64u >> 3u): { return 2u; }
    default: { return 0u; }
  }
}
)";
  const ParseResult parsed = Parse(source);
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));
  EXPECT_THAT(parsed.module.structMembers[0].type.arrayCount, Eq(8));
}

TEST(Shift, RejectsOperandsOutsideTheIntegerShiftOverloads) {
  for (const auto& [source, expected] : std::vector<std::pair<std::string, Rejection>>{
           {"fn f(x: f32, n: u32) -> f32 { return x << n; }", {ErrorCode::TypeMismatch, "<<"}},
           {"fn f(x: bool, n: u32) -> bool { return x >> n; }", {ErrorCode::TypeMismatch, ">>"}},
           {"fn f(n: u32) -> f32 { return 1.0 << n; }", {ErrorCode::TypeMismatch, "<<"}},
           // The amount is u32, never i32 or f32, whatever the value's signedness.
           {"fn f(x: u32, n: i32) -> u32 { return x << n; }", {ErrorCode::TypeMismatch, "<<"}},
           {"fn f(x: i32) -> i32 { return x >> 1i; }", {ErrorCode::TypeMismatch, ">>"}},
           {"fn f(x: u32) -> u32 { return x >> 1.0; }", {ErrorCode::TypeMismatch, ">>"}},
           {"fn f(x: u32, n: f32) -> u32 { return x << n; }", {ErrorCode::TypeMismatch, "<<"}},
           // A vector value takes a vector amount of the same width, never a scalar.
           {"fn f(x: vec2<u32>) -> vec2<u32> { return x << 1u; }", {ErrorCode::TypeMismatch, "<<"}},
           {"fn f(x: vec2<u32>) -> vec2<u32> { return x << 1; }", {ErrorCode::TypeMismatch, "<<"}},
           {"fn f(x: u32, n: vec2<u32>) -> u32 { return x >> n; }",
            {ErrorCode::TypeMismatch, ">>"}},
           {"fn f(x: vec3<i32>, n: vec2<u32>) -> vec3<i32> { return x >> n; }",
            {ErrorCode::TypeMismatch, ">>"}},
           {"fn f(x: vec2<u32>, n: vec2<i32>) -> vec2<u32> { return x << n; }",
            {ErrorCode::TypeMismatch, "<<"}},
           {"fn f(n: vec2<u32>) -> vec2<u32> { return 1 << n; }", {ErrorCode::TypeMismatch, "<<"}},
           {"fn f(x: f32) -> f32 { var y = x; y <<= 1u; return y; }",
            {ErrorCode::TypeMismatch, "<<="}},
           {"fn f(x: vec2<u32>) -> vec2<u32> { var y = x; y >>= 1u; return y; }",
            {ErrorCode::TypeMismatch, ">>="}},
           {"fn f(x: vec2<u32>) -> vec2<u32> { var y = x; y >>= 1; return y; }",
            {ErrorCode::TypeMismatch, ">>="}},
       }) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(expected));
  }
}

TEST(Shift, RejectsConstantAmountsAtOrAboveTheBitWidth) {
  for (const auto& [source, expected] : std::vector<std::pair<std::string, Rejection>>{
           {"fn f(x: u32) -> u32 { return x << 32u; }",
            {ErrorCode::InvalidConstantExpression, "32u"}},
           {"fn f(x: i32) -> i32 { return x >> 32u; }",
            {ErrorCode::InvalidConstantExpression, "32u"}},
           {"fn f(x: u32) -> u32 { return x >> 33; }",
            {ErrorCode::InvalidConstantExpression, "33"}},
           {"fn f(x: vec2<u32>) -> vec2<u32> { return x << vec2<u32>(1u, 32u); }",
            {ErrorCode::InvalidConstantExpression, "vec2<u32>(1u, 32u)"}},
           {"fn f(x: vec2<i32>) -> vec2<i32> { return x >> vec2<u32>(40u); }",
            {ErrorCode::InvalidConstantExpression, "vec2<u32>(40u)"}},
           {"const kShift = 32u;\nfn f(x: u32) -> u32 { return x << kShift; }",
            {ErrorCode::InvalidConstantExpression, "kShift"}},
           {"fn f(x: u32) -> u32 { var y = x; y >>= 32u; return y; }",
            {ErrorCode::InvalidConstantExpression, "32u"}},
           {"fn f(x: u32) -> u32 { return x << -1; }",
            {ErrorCode::InvalidConstantExpression, "-1"}},
           // A profile restriction: an abstract amount of 64 or more is rejected, not folded.
           {"const kTooWide = 1 << 64;", {ErrorCode::InvalidConstantExpression, "64"}},
           {"const kTooWide = -1 >> 64u;", {ErrorCode::InvalidConstantExpression, "64u"}},
       }) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(expected));
  }
  for (const char* source : {
           "fn f(x: u32) -> u32 { return x << 31u; }",
           "fn f(x: i32) -> i32 { return x >> 31u; }",
           "fn f(x: vec2<u32>) -> vec2<u32> { return x >> vec2<u32>(0u, 31u); }",
           "fn f(x: u32, n: u32) -> u32 { return x << n; }",
       }) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(kAccepted));
  }
}

TEST(Shift, RejectsConstantLeftShiftsThatOverflow) {
  for (const auto& [source, expected] : std::vector<std::pair<std::string, Rejection>>{
           // A signed shift may discard only copies of the result's sign bit.
           {"fn f() -> i32 { return 1073741824i << 2u; }",
            {ErrorCode::InvalidConstantExpression, "<<"}},
           {"fn f() -> i32 { return 1i << 31u; }", {ErrorCode::InvalidConstantExpression, "<<"}},
           // An unsigned shift may discard only zero bits.
           {"fn f() -> u32 { return 2147483648u << 1u; }",
            {ErrorCode::InvalidConstantExpression, "<<"}},
           {"const kOverflow = 1 << 63;", {ErrorCode::InvalidConstantExpression, "<<"}},
       }) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(expected));
  }
  for (const char* source : {
           "fn f() -> i32 { return -1i << 31u; }",
           "fn f() -> u32 { return 1073741824u << 1u; }",
           "fn f() -> u32 { return 4294967295u >> 31u; }",
           "fn f() -> i32 { return -2147483647i >> 31u; }",
       }) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(kAccepted));
  }
}

TEST(Shift, RequiresParenthesesAroundMixedOperators) {
  constexpr std::string_view kSignature = "fn f(a: u32, b: u32, c: u32) -> u32 { return ";
  for (const auto& [body, spanned] : std::vector<std::pair<std::string, std::string>>{
           {"a << b + c", "<<"},
           {"a + b << c", "<<"},
           {"a * b >> c", ">>"},
           {"a >> b * c", ">>"},
           {"a << b << c", "<<"},
           {"a >> b >> c", ">>"},
           {"a & b << c", "&"},
           {"a << b & c", "&"},
       }) {
    const std::string source = std::string(kSignature) + body + "; }";
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(Rejection{ErrorCode::UnsupportedConstruct, spanned}));
  }
  EXPECT_THAT(Diagnose("fn f(a: u32, n: u32) -> u32 { var x = a; x <<= n + 1u; return x; }"),
              Eq(Rejection{ErrorCode::UnsupportedConstruct, "<<="}));

  for (const char* body : {
           "(a << b) + c",
           "a << (b + c)",
           "(a >> b) & c",
           "(a << b) << c",
           "select(0u, 1u, a << b < c)",
           "select(0u, 1u, a < b << c)",
       }) {
    const std::string source = std::string(kSignature) + body + "; }";
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(kAccepted));
  }
  EXPECT_THAT(Diagnose("fn f(v: vec2i, n: vec2u) -> i32 { return -v.x >> n.y; }"), Eq(kAccepted));
  EXPECT_THAT(Diagnose("fn f(a: u32, n: u32) -> u32 { var x = a; x <<= (n + 1u); return x; }"),
              Eq(kAccepted));
}

TEST(Shift, RejectsAnUngroupedRightShiftAfterLessThan) {
  // WGSL's template-list discovery treats a `<` right after an identifier as a candidate list
  // start, so `a < b >> c` reads as the template list `a<b>` followed by `> c`.
  constexpr std::string_view kSignature =
      "fn g(x: u32) -> u32 { return x; }\n"
      "fn f(a: u32, b: u32, c: u32, v: vec2<u32>, i: u32) -> bool {\n"
      "  var values = array<u32, 2>(a, b);\n  return ";
  for (const char* body : {"a < b >> c", "a < 8u >> c", "a < b >> 1u", "a < (b) >> c",
                           "v.x < b >> c", "a < v.y >> c"}) {
    const std::string source = std::string(kSignature) + body + "; }";
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(Rejection{ErrorCode::UnsupportedConstruct, "<"}));
  }
  EXPECT_THAT(Diagnose("fn f(a: i32, b: i32, c: u32) -> bool { return a < -b >> c; }"),
              Eq(Rejection{ErrorCode::UnsupportedConstruct, "<"}));

  // A `<` after anything but an identifier never starts a template list.
  for (const char* body : {"a < (b >> c)", "(a < b) && (b >> c) > a", "a <= b >> c", "a > b >> c",
                           "a < b << c", "a >> b < c", "a == b >> c", "(a) < b >> c", "1u < b >> c",
                           "8u < b >> c", "values[i] < b >> c", "g(a) < b >> c"}) {
    const std::string source = std::string(kSignature) + body + "; }";
    SCOPED_TRACE(source);
    EXPECT_THAT(Diagnose(source), Eq(kAccepted));
  }
}

TEST(Shift, KeepsNestedTemplateEndsDistinctFromShifts) {
  EXPECT_THAT(Diagnose("fn f(p: ptr<function, vec2<u32>>) -> u32 { return (*p).y; }"),
              Eq(kAccepted));
  EXPECT_THAT(Diagnose("@group(0) @binding(0) var<storage, read> data: array<vec2<u32>>;\n"
                       "fn f(i: u32) -> u32 { return data[i].x >> 1u; }"),
              Eq(kAccepted));
  EXPECT_THAT(Diagnose("fn f(p: ptr<function, vec2<u32>>) -> u32 { return (*p).y << 2u; }"),
              Eq(kAccepted));
  EXPECT_THAT(Diagnose("fn f() -> vec2<u32> { var v = vec2<u32>(1u, 2u);\n"
                       "  v >>= vec2<u32>(1u); return v; }"),
              Eq(kAccepted));
}

TEST(Shift, MasksRuntimeAmountsInTheMslProjection) {
  const std::string unsignedLeft =
      Msl(ComputeModule("fn probe(value: u32, amount: u32) -> u32 { return value << amount; }",
                        "probe(gid.x, gid.y)"));
  ASSERT_THAT(unsignedLeft, Not(testing::IsEmpty()));
  EXPECT_THAT(unsignedLeft, HasSubstr(" << ("));
  EXPECT_THAT(unsignedLeft, HasSubstr(" & 31u)"));
  EXPECT_THAT(unsignedLeft, Not(HasSubstr("as_type<uint>(")));

  const std::string signedRight =
      Msl(ComputeModule("fn probe(value: i32, amount: u32) -> i32 { return value >> amount; }",
                        "probe(i32(gid.x), gid.y)"));
  ASSERT_THAT(signedRight, Not(testing::IsEmpty()));
  EXPECT_THAT(signedRight, HasSubstr(" >> ("));
  EXPECT_THAT(signedRight, HasSubstr(" & 31u)"));
  EXPECT_THAT(signedRight, Not(HasSubstr("as_type<uint>(")));

  const std::string vectorLeft = Msl(ComputeModule(
      "fn probe(value: vec3<u32>, amount: vec3<u32>) -> vec3<u32> { return value << amount; }",
      "probe(gid, gid).z"));
  ASSERT_THAT(vectorLeft, Not(testing::IsEmpty()));
  EXPECT_THAT(vectorLeft, HasSubstr(" & uint3(31u))"));
}

TEST(Shift, ShiftsSignedValuesLeftThroughUnsignedBitsInMsl) {
  // A negative or overflowing signed left shift is undefined in MSL, so the bits move as uint.
  const std::string signedLeft =
      Msl(ComputeModule("fn probe(value: i32, amount: u32) -> i32 { return value << amount; }",
                        "probe(i32(gid.x), gid.y)"));
  ASSERT_THAT(signedLeft, Not(testing::IsEmpty()));
  EXPECT_THAT(signedLeft, HasSubstr("as_type<int>((as_type<uint>("));
  EXPECT_THAT(signedLeft, HasSubstr(" & 31u)"));
}

TEST(Shift, LeavesLiteralAmountsUnmasked) {
  // A literal amount is already validated below the bit width.
  const std::string source = ComputeModule(
      "const kShift = 4u;\n"
      "fn probe(value: u32) -> u32 { return (value >> 8u) + (value << kShift); }",
      "probe(gid.x)");
  const std::string msl = Msl(source);
  ASSERT_THAT(msl, Not(testing::IsEmpty()));
  EXPECT_THAT(msl, HasSubstr(" >> 8u)"));
  EXPECT_THAT(msl, HasSubstr(" << 4u)"));
  EXPECT_THAT(msl, Not(HasSubstr("31u")));

  const std::vector<uint32_t> opcodes = Opcodes(Spirv(source));
  ASSERT_THAT(opcodes, Not(testing::IsEmpty()));
  EXPECT_THAT(opcodes, Contains(kOpShiftRightLogical));
  EXPECT_THAT(opcodes, Contains(kOpShiftLeftLogical));
  EXPECT_THAT(opcodes, Not(Contains(kOpBitwiseAnd)));
}

TEST(Shift, SelectsSpirvShiftsBySignedness) {
  const std::vector<uint32_t> unsignedRight = Opcodes(
      Spirv(ComputeModule("fn probe(value: u32, amount: u32) -> u32 { return value >> amount; }",
                          "probe(gid.x, gid.y)")));
  ASSERT_THAT(unsignedRight, Not(testing::IsEmpty()));
  EXPECT_THAT(unsignedRight, Contains(kOpShiftRightLogical));
  EXPECT_THAT(unsignedRight, Not(Contains(kOpShiftRightArithmetic)));
  EXPECT_THAT(unsignedRight, Contains(kOpBitwiseAnd));

  const std::vector<uint32_t> signedRight = Opcodes(
      Spirv(ComputeModule("fn probe(value: i32, amount: u32) -> i32 { return value >> amount; }",
                          "probe(i32(gid.x), gid.y)")));
  ASSERT_THAT(signedRight, Not(testing::IsEmpty()));
  EXPECT_THAT(signedRight, Contains(kOpShiftRightArithmetic));
  EXPECT_THAT(signedRight, Not(Contains(kOpShiftRightLogical)));
  EXPECT_THAT(signedRight, Contains(kOpBitwiseAnd));

  const std::vector<uint32_t> vectorLeft = Opcodes(Spirv(ComputeModule(
      "fn probe(value: vec2<i32>, amount: vec2<u32>) -> vec2<i32> { return value << amount; }",
      "probe(vec2<i32>(gid.xy), gid.yx).x")));
  ASSERT_THAT(vectorLeft, Not(testing::IsEmpty()));
  EXPECT_THAT(vectorLeft, Contains(kOpShiftLeftLogical));
  EXPECT_THAT(vectorLeft, Contains(kOpBitwiseAnd));
}

TEST(Shift, LowersCompoundAssignmentLikeTheSpelledOutForm) {
  const auto source = [](std::string_view statements) {
    return ComputeModule(
        "fn probe(value: vec2<i32>, amount: u32) -> vec2<i32> {\n"
        "  var result = value;\n  var scalar = u32(value.x);\n" +
            std::string(statements) + "  return result + vec2<i32>(i32(scalar));\n}\n",
        "probe(vec2<i32>(gid.xy), gid.z).y");
  };
  const std::string compound = source(
      "  result <<= vec2<u32>(amount);\n  result >>= vec2<u32>(1u, amount);\n"
      "  scalar >>= amount;\n  scalar <<= 3u;\n");
  const std::string spelled = source(
      "  result = result << vec2<u32>(amount);\n  result = result >> vec2<u32>(1u, amount);\n"
      "  scalar = scalar >> amount;\n  scalar = scalar << 3u;\n");
  ASSERT_THAT(Diagnose(compound), Eq(kAccepted));
  ASSERT_THAT(Diagnose(spelled), Eq(kAccepted));
  EXPECT_THAT(Msl(compound), Not(testing::IsEmpty()));
  EXPECT_THAT(Msl(compound), Eq(Msl(spelled)));
  EXPECT_THAT(Spirv(compound), Not(testing::IsEmpty()));
  EXPECT_THAT(Spirv(compound), Eq(Spirv(spelled)));
}

TEST(Shift, KeepsAuthoredShiftsInTheWgslProjection) {
  const std::string source = ComputeModule(
      "fn probe(value: u32, amount: u32) -> u32 {\n"
      "  var result = value >> amount;\n  result <<= 2u;\n  return result;\n}\n",
      "probe(gid.x, gid.y)");
  ASSERT_THAT(Diagnose(source), Eq(kAccepted));
  const std::string wgsl = Wgsl(source);
  ASSERT_THAT(wgsl, Not(testing::IsEmpty()));
  EXPECT_THAT(wgsl, HasSubstr("var result = value >> amount;"));
  EXPECT_THAT(wgsl, HasSubstr("result <<= 2u;"));
  // The projection parses to the same module and so to the same native bytes.
  EXPECT_THAT(Msl(wgsl), Eq(Msl(source)));
  EXPECT_THAT(Spirv(wgsl), Eq(Spirv(source)));
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
