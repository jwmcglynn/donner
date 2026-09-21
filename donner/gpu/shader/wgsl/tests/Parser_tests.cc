#include "donner/gpu/shader/wgsl/Parser.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/shader/programs/GaussianBlurSource.h"

namespace donner::gpu::shader::wgsl {
namespace {

TEST(Parser, ParsesGaussianBlurIntoTheExpectedTypedArenas) {
  constexpr ParseResult parsed = Parse(programs::kGaussianBlurSource.view());
  static_assert(parsed.hasResult());

  EXPECT_EQ(parsed.module.structCount, 1u);
  EXPECT_EQ(parsed.module.bindingCount, 3u);
  EXPECT_EQ(parsed.module.symbolCount, 22u);
  EXPECT_EQ(parsed.module.expressionCount, 261u);
  EXPECT_EQ(parsed.module.statementCount, 43u);
  EXPECT_EQ(parsed.module.functionCount, 3u);
  EXPECT_EQ(parsed.module.name(parsed.module.functions[2].name), "cs_main");
  EXPECT_EQ(parsed.module.functions[2].stage, Stage::Compute);
  EXPECT_EQ(parsed.module.functions[2].workgroupSize, (std::array<uint32_t, 3>{8, 8, 1}));
}

TEST(Parser, RejectsSourceAndArenaBudgetViolations) {
  std::array<char, ModuleLimits::kMaxSourceBytes + 1> tooLarge = {};
  tooLarge.fill(' ');
  EXPECT_EQ(Parse({tooLarge.data(), tooLarge.size()}).diagnostic.code, ErrorCode::SourceTooLarge);

  constexpr std::string_view kNonAscii = "\x80";
  EXPECT_EQ(Parse(kNonAscii).diagnostic.code, ErrorCode::NonAsciiSource);
}

TEST(Parser, EnforcesLiteralAndSemanticRules) {
  constexpr std::string_view kUntypedLiteral = R"(
@compute @workgroup_size(1)
fn cs(@builtin(global_invocation_id) gid: vec3<u32>) { let value = 1; }
)";
  constexpr std::string_view kInvalidReturn = R"(
@compute @workgroup_size(1)
fn cs(@builtin(global_invocation_id) gid: vec3<u32>) -> i32 { return 0i; }
)";
  constexpr std::string_view kDuplicateName = R"(
fn same() {}
fn same() {}
)";
  constexpr std::string_view kConstantDivideByZero = R"(
fn divide() -> i32 { return 1i / 0i; }
)";
  constexpr std::string_view kDynamicDivideIsAccepted = R"(
fn divide(value: i32, denominator: i32) -> i32 { return value / denominator; }
@compute @workgroup_size(1)
fn cs(@builtin(global_invocation_id) gid: vec3<u32>) {}
)";

  EXPECT_EQ(Parse(kUntypedLiteral).diagnostic.code, ErrorCode::None);
  EXPECT_EQ(Parse(kInvalidReturn).diagnostic.code, ErrorCode::InvalidReturn);
  EXPECT_EQ(Parse(kDuplicateName).diagnostic.code, ErrorCode::DuplicateName);
  EXPECT_EQ(Parse(kConstantDivideByZero).diagnostic.code, ErrorCode::InvalidConstantExpression);
  EXPECT_TRUE(Parse(kDynamicDivideIsAccepted).hasResult());
}

TEST(Parser, RoundsFractionalF32LiteralsToRepresentableValues) {
  struct Case {
    const char* source;
    uint32_t bits;
  };
  for (const auto& item : {Case{"fn f() { let value = 8388608.5f; }", 0x4b000000},
                           Case{"fn f() { let value = 4194304.25f; }", 0x4a800000},
                           Case{"fn f() { let value = 4194304.75f; }", 0x4a800002}}) {
    SCOPED_TRACE(item.source);
    const auto result = Parse(item.source);
    ASSERT_EQ(result.diagnostic.code, ErrorCode::None);
    EXPECT_EQ(result.module.expressions[0].payload, item.bits);
  }
}

TEST(Parser, RejectsReversedStaticClampBounds) {
  constexpr std::string_view kReversedScalarBounds = R"(
fn scalar(value: f32) -> f32 { return clamp(value, 1f, 0f); }
)";
  constexpr std::string_view kReversedVectorBounds = R"(
fn vector(value: vec2<f32>) -> vec2<f32> {
  return clamp(value, vec2<f32>(0f, 2f), vec2<f32>(1f, 1f));
}
)";
  constexpr std::string_view kOrderedBounds = R"(
fn scalar(value: f32) -> f32 { return clamp(value, 0f, 1f); }
)";

  EXPECT_EQ(Parse(kReversedScalarBounds).diagnostic.code, ErrorCode::InvalidConstantExpression);
  EXPECT_EQ(Parse(kReversedVectorBounds).diagnostic.code, ErrorCode::InvalidConstantExpression);
  EXPECT_TRUE(Parse(kOrderedBounds).hasResult());
}

TEST(Parser, RejectsStaticVectorArithmeticAndBuiltins) {
  constexpr std::string_view kVectorDivideByZero = R"(
fn vector() -> vec2<f32> { return vec2<f32>(1f, 1f) / vec2<f32>(0f, 0f); }
)";
  constexpr std::string_view kStaticVectorExp = R"(
fn vector() -> vec2<f32> { return exp(vec2<f32>(16777216f, 16777216f)); }
)";

  EXPECT_EQ(Parse(kVectorDivideByZero).diagnostic.code, ErrorCode::InvalidConstantExpression);
  EXPECT_EQ(Parse(kStaticVectorExp).diagnostic.code, ErrorCode::InvalidConstantExpression);
}

TEST(Parser, RejectsReservedDeclarationNames) {
  constexpr std::string_view kKeywordMember = R"(struct S { if: f32, })";
  constexpr std::string_view kKeywordLocal = R"(fn f() { let let = 1i; })";
  constexpr std::string_view kUnderscoreFunction = R"(fn _() {})";
  constexpr std::string_view kDoubleUnderscoreFunction = R"(fn __internal() {})";

  EXPECT_EQ(Parse(kKeywordMember).diagnostic.code, ErrorCode::InvalidIdentifier);
  EXPECT_EQ(Parse(kKeywordLocal).diagnostic.code, ErrorCode::InvalidIdentifier);
  EXPECT_EQ(Parse(kUnderscoreFunction).diagnostic.code, ErrorCode::InvalidIdentifier);
  EXPECT_EQ(Parse(kDoubleUnderscoreFunction).diagnostic.code, ErrorCode::InvalidIdentifier);
}

TEST(Parser, RejectsLocalAliasesOfResources) {
  constexpr std::string_view kResourceAlias = R"(
@group(0) @binding(0) var input: texture_2d<f32>;
fn helper() { let input = 1i; }
)";

  EXPECT_EQ(Parse(kResourceAlias).diagnostic.code, ErrorCode::DuplicateName);
}

TEST(Parser, RejectsInvalidModuleDeclarationsAndAttributes) {
  constexpr std::string_view kVectorElementIsNotScalar = R"(
fn f(v: vec2<f32>) { let value = vec2<vec2<f32>>(v); }
)";
  constexpr std::string_view kDuplicateCompute = R"(
@compute @compute @workgroup_size(1)
fn cs(@builtin(global_invocation_id) gid: vec3<u32>) {}
)";
  constexpr std::string_view kDuplicateWorkgroupSize = R"(
@compute @workgroup_size(1) @workgroup_size(1)
fn cs(@builtin(global_invocation_id) gid: vec3<u32>) {}
)";
  constexpr std::string_view kDuplicateBuiltin = R"(
@compute @workgroup_size(1)
fn cs(@builtin(global_invocation_id) @builtin(global_invocation_id) gid: vec3<u32>) {}
)";
  constexpr std::string_view kOverflowingWorkgroupProduct = R"(
@compute @workgroup_size(2147483648, 2147483648, 4)
fn cs(@builtin(global_invocation_id) gid: vec3<u32>) {}
)";
  constexpr std::string_view kEmptyStruct = R"(struct S {})";
  constexpr std::string_view kStructFunctionCollision = R"(
struct S { x: f32, }
fn S() {}
)";
  constexpr std::string_view kBindingFunctionCollision = R"(
@group(0) @binding(0) var value: texture_2d<f32>;
fn value() {}
)";
  constexpr std::string_view kBindingStructCollision = R"(
@group(0) @binding(0) var value: texture_2d<f32>;
struct value { x: f32, }
)";

  EXPECT_EQ(Parse(kVectorElementIsNotScalar).diagnostic.code, ErrorCode::UnknownType);
  EXPECT_EQ(Parse(kDuplicateCompute).diagnostic.code, ErrorCode::InvalidAttribute);
  EXPECT_EQ(Parse(kDuplicateWorkgroupSize).diagnostic.code, ErrorCode::InvalidAttribute);
  EXPECT_EQ(Parse(kDuplicateBuiltin).diagnostic.code, ErrorCode::InvalidAttribute);
  EXPECT_EQ(Parse(kOverflowingWorkgroupProduct).diagnostic.code, ErrorCode::InvalidAttribute);
  EXPECT_EQ(Parse(kEmptyStruct).diagnostic.code, ErrorCode::InvalidLayout);
  EXPECT_EQ(Parse(kStructFunctionCollision).diagnostic.code, ErrorCode::DuplicateName);
  EXPECT_EQ(Parse(kBindingFunctionCollision).diagnostic.code, ErrorCode::DuplicateName);
  EXPECT_EQ(Parse(kBindingStructCollision).diagnostic.code, ErrorCode::DuplicateName);
}

TEST(Parser, RejectsResourceValuesAndModuleNameShadowing) {
  constexpr std::string_view kResourceEquality = R"(
@group(0) @binding(0) var input: texture_2d<f32>;
fn f() { let equal = input == input; }
)";
  constexpr std::string_view kResourceSelect = R"(
@group(0) @binding(0) var input: texture_2d<f32>;
fn f() { let selected = select(input, input, true); }
)";
  constexpr std::string_view kCalleeShadow = R"(
fn f(value: i32) -> i32 { return value; }
fn g() { let f = 1i; let value = f(2i); }
)";

  EXPECT_EQ(Parse(kResourceEquality).diagnostic.code, ErrorCode::TypeMismatch);
  EXPECT_EQ(Parse(kResourceSelect).diagnostic.code, ErrorCode::InvalidCall);
  EXPECT_EQ(Parse(kCalleeShadow).diagnostic.code, ErrorCode::DuplicateName);
}

TEST(Parser, ParsesFixedF32StorageArrays) {
  constexpr std::string_view kStorageArray = R"(
struct Params {
  orderX: i32,
  orderY: i32,
  targetX: i32,
  targetY: i32,
  divisor: f32,
  bias: f32,
  edgeMode: u32,
  preserveAlpha: u32,
  coefficients: array<f32, 25>,
}
@group(0) @binding(2) var<storage, read> params: Params;
fn coefficient(index: i32) -> f32 { return params.coefficients[index]; }
)";
  constexpr ParseResult parsed = Parse(kStorageArray);

  static_assert(parsed.hasResult());
  EXPECT_EQ(parsed.module.bindings[0].kind, BindingKind::ReadOnlyStorage);
  EXPECT_EQ(parsed.module.structs[0].size, 132u);
  EXPECT_EQ(parsed.module.structMembers[8].offset, 32u);
  EXPECT_EQ(parsed.module.structMembers[8].arrayStride, 4u);
  bool hasIndex = false;
  for (uint16_t i = 0; i < parsed.module.expressionCount; ++i) {
    hasIndex |= parsed.module.expressions[i].kind == ExpressionKind::Index;
  }
  EXPECT_TRUE(hasIndex);
}

TEST(Parser, AppliesRuntimeBindingLimitToReadOnlyStorage) {
  static_assert(gpu::kMaxBindings == 32, "The sources below name the cap and the index past it");
  constexpr std::string_view kHighestBinding = R"(
struct Params { coefficients: array<f32, 1>, }
@group(0) @binding(31) var<storage, read> params: Params;
)";
  constexpr std::string_view kBindingPastTheCap = R"(
struct Params { coefficients: array<f32, 1>, }
@group(0) @binding(32) var<storage, read> params: Params;
)";

  EXPECT_EQ(Parse(kHighestBinding).diagnostic.code, ErrorCode::None);
  EXPECT_EQ(Parse(kBindingPastTheCap).diagnostic.code, ErrorCode::InvalidBinding);
}

TEST(Parser, RejectsUnsupportedArrayUsesAndStaticOutOfBoundsIndices) {
  constexpr std::string_view kUniformArray = R"(
struct Params { coefficients: array<f32, 25>, }
@group(0) @binding(0) var<uniform> params: Params;
)";
  constexpr std::string_view kStaticOutOfBounds = R"(
struct Params { coefficients: array<f32, 25>, }
@group(0) @binding(0) var<storage, read> params: Params;
fn coefficient() -> f32 { return params.coefficients[25i]; }
)";
  constexpr std::string_view kNegativeStaticIndex = R"(
struct Params { coefficients: array<f32, 25>, }
@group(0) @binding(0) var<storage, read> params: Params;
fn coefficient() -> f32 { return params.coefficients[-1i]; }
)";
  constexpr std::string_view kLocalArray = R"(
fn f() { var values: array<f32, 25> = array<f32, 25>(); }
)";
  constexpr std::string_view kSwizzledStaticIndex = R"(
struct Params { coefficients: array<f32, 25>, }
@group(0) @binding(0) var<storage, read> params: Params;
fn coefficient() -> f32 { return params.coefficients[vec2<i32>(25i).x]; }
)";

  EXPECT_EQ(Parse(kUniformArray).diagnostic.code, ErrorCode::UnsupportedConstruct);
  EXPECT_EQ(Parse(kStaticOutOfBounds).diagnostic.code, ErrorCode::InvalidConstantExpression);
  EXPECT_EQ(Parse(kNegativeStaticIndex).diagnostic.code, ErrorCode::InvalidConstantExpression);
  EXPECT_EQ(Parse(kLocalArray).diagnostic.code, ErrorCode::None);
  EXPECT_EQ(Parse(kSwizzledStaticIndex).diagnostic.code, ErrorCode::InvalidConstantExpression);
}

TEST(Parser, BoundsLeftAssociativeExpressionTreeDepth) {
  std::string source = "fn f(x: i32) -> i32 { return x";
  for (uint16_t i = 0; i < ModuleLimits::kMaxNesting; ++i) source += " + 1i";
  source += "; }";
  EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::NestingLimit);
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
