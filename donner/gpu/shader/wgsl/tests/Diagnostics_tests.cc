#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/shader/wgsl/Parser.h"

namespace donner::gpu::shader::wgsl {
namespace {

/// One minimal offending source and the exact diagnostic Parse() must report for it.
struct DiagnosticCase {
  const char* name;
  const char* source;
  ErrorCode expected;
};

/// Asserts that parsing rejects `source` with exactly `expected` and yields no module.
void ExpectRejected(std::string_view source, ErrorCode expected) {
  ASSERT_LE(source.size(), ModuleLimits::kMaxSourceBytes);
  const ParseResult parsed = Parse(source);
  EXPECT_EQ(parsed.diagnostic.code, expected) << "actual code " << unsigned(parsed.diagnostic.code);
  EXPECT_FALSE(parsed.hasResult());
}

/// Matches a WGSL source that Parse() accepts, naming the diagnostic when it does not.
MATCHER(ParsedSuccessfully, "parses into a module without a diagnostic") {
  const ParseResult parsed = Parse(std::string_view(arg));
  *result_listener << "diagnostic code " << unsigned(parsed.diagnostic.code) << " over bytes ["
                   << parsed.diagnostic.span.begin << ", " << parsed.diagnostic.span.end << ")";
  return parsed.hasResult();
}

/// Matches a WGSL source that Parse() rejects with exactly `expected`, naming the actual code.
MATCHER_P(RejectedWithCode, expected,
          "is rejected with diagnostic code " + testing::PrintToString(unsigned(expected))) {
  const ParseResult parsed = Parse(std::string_view(arg));
  *result_listener << "diagnostic code " << unsigned(parsed.diagnostic.code) << " over bytes ["
                   << parsed.diagnostic.span.begin << ", " << parsed.diagnostic.span.end
                   << "), produced a module: " << (parsed.hasResult() ? "yes" : "no");
  return parsed.diagnostic.code == expected && !parsed.hasResult();
}

/// A module declaring one texture at `binding` in group zero.
std::string TextureBindingSource(uint32_t binding) {
  return "@group(0) @binding(" + std::to_string(binding) + ") var t: texture_2d<f32>;\n";
}

/// A compute entry point with the given declared workgroup size.
std::string WorkgroupSource(uint32_t x, uint32_t y, uint32_t z) {
  return "@compute @workgroup_size(" + std::to_string(x) + ", " + std::to_string(y) + ", " +
         std::to_string(z) + ")\nfn cs(@builtin(global_invocation_id) gid: vec3<u32>) {}\n";
}

const DiagnosticCase kSemanticCases[] = {
    {"top_level_let", "let value = 1i;", ErrorCode::UnexpectedToken},
    {"unknown_punctuation", "fn f() { let value = #; }", ErrorCode::UnexpectedToken},
    {"missing_semicolon", "fn f() { let value = 1i }", ErrorCode::UnexpectedToken},
    {"switch_clause_without_case", "fn f(x: i32) { switch (x) { 1i: {} default: {} } }",
     ErrorCode::UnexpectedToken},

    {"workgroup_size_not_a_number",
     "@compute @workgroup_size(one) fn cs(@builtin(global_invocation_id) gid: vec3<u32>) {}",
     ErrorCode::InvalidLiteral},
    {"u32_literal_out_of_range", "fn f() { let value = 4294967296u; }", ErrorCode::InvalidLiteral},
    {"i32_literal_out_of_range", "fn f() { let value = 2147483648i; }", ErrorCode::InvalidLiteral},

    {"unknown_local_name", "fn f() -> i32 { return missing; }", ErrorCode::UnknownName},
    {"unknown_callee", "fn f() -> i32 { return missing(); }", ErrorCode::UnknownName},
    {"unknown_struct_member", "struct S { x: f32, }\nfn f(s: S) -> f32 { return s.y; }",
     ErrorCode::UnknownName},

    {"same_group_and_binding",
     "@group(0) @binding(0) var a: texture_2d<f32>;\n@group(0) @binding(0) var b: texture_2d<f32>;",
     ErrorCode::DuplicateBinding},

    {"assign_to_let", "fn f() { let value = 1i; value = 2i; }", ErrorCode::ImmutableAssignment},
    {"assign_to_parameter", "fn f(value: i32) { value = 2i; }", ErrorCode::ImmutableAssignment},

    {"integer_if_condition", "fn f() { if (1i) {} }", ErrorCode::InvalidCondition},
    {"float_if_condition", "fn f(x: f32) { if (x) {} }", ErrorCode::InvalidCondition},

    {"statement_after_return", "fn f() -> i32 { return 1i; return 2i; }",
     ErrorCode::UnreachableStatement},
    {"statement_after_break",
     "fn f() { for (var i: i32 = 0i; i < 4i; i = i + 1i) { break; i = 0i; } }",
     ErrorCode::UnreachableStatement},

    {"switch_without_default", "fn f(x: i32) { switch (x) { case 1i: {} } }",
     ErrorCode::InvalidSwitch},
    {"switch_without_clauses", "fn f(x: i32) { switch (x) {} }", ErrorCode::InvalidSwitch},
    {"switch_duplicate_default", "fn f(x: i32) { switch (x) { default: {} default: {} } }",
     ErrorCode::InvalidSwitch},
    {"switch_duplicate_label",
     "fn f(x: i32) { switch (x) { case 1i: {} case 1i: {} default: {} } }",
     ErrorCode::InvalidSwitch},
    {"switch_float_selector", "fn f(x: f32) { switch (x) { default: {} } }",
     ErrorCode::InvalidSwitch},
};

TEST(Diagnostics, EvaluatesTypedIntegerArithmeticBeforeCheckingClampBounds) {
  struct ClampCase {
    const char* name;
    const char* type;
    const char* low;
    const char* high;
    ErrorCode expected;
  };
  const ClampCase cases[] = {
      {"signed_add_subtract", "i32", "2i + 3i", "8i - 1i", ErrorCode::None},
      {"signed_multiply_divide", "i32", "2i * 3i", "20i / 2i", ErrorCode::None},
      {"signed_remainder_negation", "i32", "17i % 5i", "-(-3i)", ErrorCode::None},
      {"signed_negative_bounds", "i32", "-9i / 2i", "-5i % 3i", ErrorCode::None},
      {"signed_reversed", "i32", "8i / 2i", "2i + 1i", ErrorCode::InvalidConstantExpression},
      {"signed_add_overflow", "i32", "2147483647i + 1i", "2147483647i",
       ErrorCode::InvalidConstantExpression},
      {"signed_subtract_underflow", "i32", "(-2147483647i - 1i) - 1i", "0i",
       ErrorCode::InvalidConstantExpression},
      {"signed_multiply_overflow", "i32", "1073741824i * 2i", "2147483647i",
       ErrorCode::InvalidConstantExpression},
      {"signed_divide_zero", "i32", "3i / 0i", "4i", ErrorCode::InvalidConstantExpression},
      {"signed_remainder_zero", "i32", "3i % 0i", "4i", ErrorCode::InvalidConstantExpression},
      {"unsigned_add_subtract", "u32", "2u + 3u", "8u - 1u", ErrorCode::None},
      {"unsigned_multiply_divide", "u32", "2u * 3u", "20u / 2u", ErrorCode::None},
      {"unsigned_remainder", "u32", "17u % 5u", "3u", ErrorCode::None},
      {"unsigned_reversed", "u32", "8u / 2u", "2u + 1u", ErrorCode::InvalidConstantExpression},
      {"unsigned_add_overflow", "u32", "4294967295u + 1u", "4294967295u",
       ErrorCode::InvalidConstantExpression},
      {"unsigned_subtract_underflow", "u32", "0u - 1u", "1u", ErrorCode::InvalidConstantExpression},
      {"unsigned_multiply_overflow", "u32", "2147483648u * 2u", "4294967295u",
       ErrorCode::InvalidConstantExpression},
      {"unsigned_divide_zero", "u32", "3u / 0u", "4u", ErrorCode::InvalidConstantExpression},
      {"unsigned_remainder_zero", "u32", "3u % 0u", "4u", ErrorCode::InvalidConstantExpression},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.name);
    const std::string source = std::string("fn f(value: ") + item.type + ") -> " + item.type +
                               " { return clamp(value, " + item.low + ", " + item.high + "); }";
    if (item.expected == ErrorCode::None) {
      EXPECT_THAT(source, ParsedSuccessfully());
    } else {
      EXPECT_THAT(source, RejectedWithCode(item.expected));
    }
  }
}

TEST(Diagnostics, ChecksEveryComponentOfNegatedAndSplatClampBounds) {
  EXPECT_THAT(R"(fn f(value: vec2<f32>) -> vec2<f32> {
    return clamp(value, -vec2<f32>(2f, 3f), vec2<f32>(1f));
  })",
              ParsedSuccessfully());
  EXPECT_THAT(R"(fn f(value: vec2<f32>) -> vec2<f32> {
    return clamp(value, -vec2<f32>(2f, 3f), -vec2<f32>(3f));
  })",
              RejectedWithCode(ErrorCode::InvalidConstantExpression));
}

TEST(Diagnostics, ReportsExactCodeForMinimalSemanticViolations) {
  for (const DiagnosticCase& item : kSemanticCases) {
    SCOPED_TRACE(item.name);
    ExpectRejected(item.source, item.expected);
  }
}

const DiagnosticCase kInvalidWgslCases[] = {
    {"stage_on_struct", "@compute struct S { value: f32, }", ErrorCode::InvalidAttribute},
    {"binding_on_const", "@binding(0) const value = 1i;", ErrorCode::InvalidAttribute},
    {"duplicate_stage", "@compute @fragment fn f() {}", ErrorCode::InvalidAttribute},
    {"duplicate_location", "struct S { @location(0) @location(1) value: f32, }",
     ErrorCode::InvalidAttribute},
    {"duplicate_interpolation", "struct S { @interpolate(flat) @interpolate(flat) value: i32, }",
     ErrorCode::InvalidAttribute},
    {"malformed_diagnostic_attribute", "@diagnostic fn f() {}", ErrorCode::UnsupportedConstruct},
    {"duplicate_workgroup_size", "@compute @workgroup_size(1) @workgroup_size(2) fn f() {}",
     ErrorCode::InvalidAttribute},
    {"duplicate_group", "@group(0) @group(0) @binding(0) var t: texture_2d<f32>;",
     ErrorCode::InvalidAttribute},
    {"duplicate_binding", "@group(0) @binding(0) @binding(1) var t: texture_2d<f32>;",
     ErrorCode::InvalidAttribute},
    {"duplicate_builtin", "struct S { @builtin(position) @builtin(position) value: vec4<f32>, }",
     ErrorCode::InvalidAttribute},
    {"unknown_builtin", "struct S { @builtin(unknown) value: f32, }", ErrorCode::InvalidAttribute},
    {"attribute_unsigned_overflow", "@compute @workgroup_size(4294967296) fn f() {}",
     ErrorCode::InvalidLiteral},
    {"reserved_struct_name", "struct __private { value: f32, }", ErrorCode::InvalidIdentifier},
    {"duplicate_struct_member", "struct S { value: f32, value: f32, }", ErrorCode::DuplicateName},
    {"stage_on_member", "struct S { @compute value: f32, }", ErrorCode::InvalidAttribute},
    {"reserved_binding_name", "@group(0) @binding(0) var __private: texture_2d<f32>;",
     ErrorCode::InvalidIdentifier},
    {"binding_without_attributes", "var t: texture_2d<f32>;", ErrorCode::InvalidAttribute},
    {"constant_struct_collision", "const S = 1i; struct S { value: f32, }",
     ErrorCode::DuplicateName},
    {"binding_struct_collision",
     "struct S { value: f32, } @group(0) @binding(0) var S: texture_2d<f32>;",
     ErrorCode::DuplicateName},
    {"binding_binding_collision",
     "@group(0) @binding(0) var t: texture_2d<f32>; @group(0) @binding(1) var t: texture_2d<f32>;",
     ErrorCode::DuplicateName},
    {"duplicate_parameter", "fn f(value: f32, value: f32) {}", ErrorCode::DuplicateName},
    {"duplicate_local", "fn f() { let value = 1i; let value = 2i; }", ErrorCode::DuplicateName},
    {"vector_vector_element", "fn f(value: vec2<vec2<f32>>) {}", ErrorCode::UnknownType},
    {"matrix_integer_element", "fn f(value: mat2x2<i32>) {}", ErrorCode::UnknownType},
    {"array_zero_count", "fn f(value: array<f32, 0>) {}", ErrorCode::UnknownType},
    {"array_wrong_element", "fn f() { let value = array<f32, 2>(1f, true); }",
     ErrorCode::TypeMismatch},
    {"array_wrong_count", "fn f() { let value = array<f32, 2>(1f); }", ErrorCode::InvalidCall},
    {"runtime_array_construction", "fn f() { let value = array<f32>(); }",
     ErrorCode::UnsupportedConstruct},
    {"scalar_index", "fn f(value: f32) { let result = value[0i]; }", ErrorCode::TypeMismatch},
    {"vector_float_index", "fn f(value: vec2<f32>) { let result = value[0f]; }",
     ErrorCode::TypeMismatch},
    {"scalar_member", "fn f(value: f32) { let result = value.x; }", ErrorCode::TypeMismatch},
    {"convert_vector", "fn f(value: vec2<f32>) { let result = i32(value); }",
     ErrorCode::TypeMismatch},
    {"negate_i32_minimum", "fn f() { let result = -(-2147483647i - 1i); }",
     ErrorCode::InvalidConstantExpression},
    {"negate_abstract_minimum", "fn f() { let result = -(-9223372036854775807 - 1); }",
     ErrorCode::InvalidConstantExpression},
    {"not_abstract_integer", "fn f() { let result = !1; }", ErrorCode::TypeMismatch},
    {"divide_i32_minimum", "fn f() { let result = (-2147483647i - 1i) / -1i; }",
     ErrorCode::InvalidConstantExpression},
    {"texture_store_coordinates",
     "@group(0) @binding(0) var t: texture_storage_2d<rgba32float, write>; "
     "@compute @workgroup_size(1) fn f() { "
     "textureStore(t, vec2<f32>(0f), vec4<f32>(0f)); }",
     ErrorCode::InvalidCall},
};

const DiagnosticCase kUnsupportedProfileCases[] = {
    {"location_out_of_range", "struct S { @location(16) value: f32, }",
     ErrorCode::InvalidAttribute},
    {"unsupported_interpolation", "struct S { @interpolate(linear) value: f32, }",
     ErrorCode::UnsupportedConstruct},
    {"local_binding_collision",
     "@group(0) @binding(0) var t: texture_2d<f32>; fn f() { let t = 1i; }",
     ErrorCode::DuplicateName},
    {"sampled_texture_integer", "@group(0) @binding(0) var t: texture_2d<i32>;",
     ErrorCode::UnknownType},
    {"storage_texture_format", "@group(0) @binding(0) var t: texture_storage_2d<r32float, write>;",
     ErrorCode::UnknownType},
    {"storage_texture_access",
     "@group(0) @binding(0) var t: texture_storage_2d<rgba32float, read>;", ErrorCode::UnknownType},
    {"fixed_struct_array", "struct S { value: f32, } fn f(value: array<S, 2>) {}",
     ErrorCode::UnsupportedConstruct},
    {"runtime_struct_array_member", "struct S { value: f32, } struct T { value: array<S>, }",
     ErrorCode::UnsupportedConstruct},
    {"array_non_numeric_element", "fn f(value: array<bool, 2>) {}", ErrorCode::UnknownType},
    {"module_array_construction", "const value = array<f32, 2>(1f, 2f);",
     ErrorCode::UnsupportedConstruct},
    {"convert_typed_constant", "fn f() { let result = i32(1u); }",
     ErrorCode::InvalidConstantExpression},
    {"texture_store_helper",
     "@group(0) @binding(0) var t: texture_storage_2d<rgba32float, write>; "
     "fn f() { textureStore(t, vec2<i32>(0i), vec4<f32>(0f)); }",
     ErrorCode::UnsupportedConstruct},
};

class InvalidWgslDiagnostic : public testing::TestWithParam<DiagnosticCase> {};

TEST_P(InvalidWgslDiagnostic, RejectsMalformedDeclarationOrExpression) {
  const DiagnosticCase& item = GetParam();
  EXPECT_THAT(item.source, RejectedWithCode(item.expected));
}

INSTANTIATE_TEST_SUITE_P(LanguageValidation, InvalidWgslDiagnostic,
                         testing::ValuesIn(kInvalidWgslCases),
                         [](const testing::TestParamInfo<DiagnosticCase>& info) {
                           return info.param.name;
                         });

class SupportedProfileDiagnostic : public testing::TestWithParam<DiagnosticCase> {};

TEST_P(SupportedProfileDiagnostic, RejectsConstructOutsideTheSupportedProfile) {
  const DiagnosticCase& item = GetParam();
  EXPECT_THAT(item.source, RejectedWithCode(item.expected));
}

INSTANTIATE_TEST_SUITE_P(BoundedLanguageProfile, SupportedProfileDiagnostic,
                         testing::ValuesIn(kUnsupportedProfileCases),
                         [](const testing::TestParamInfo<DiagnosticCase>& info) {
                           return info.param.name;
                         });

TEST(Diagnostics, RejectsInvalidValueBuiltinArguments) {
  struct BuiltinCase {
    const char* name;
    const char* parameters;
    const char* call;
  };
  const BuiltinCase cases[] = {
      {"dot_scalar", "a: f32", "dot(a, a)"},
      {"dot_wrong_width", "a: vec2<f32>, b: vec3<f32>", "dot(a, b)"},
      {"length_integer", "a: vec2<i32>", "length(a)"},
      {"normalize_extra_argument", "a: vec2<f32>", "normalize(a, a)"},
      {"pow_integer", "a: i32", "pow(a, a)"},
      {"pow_wrong_width", "a: vec2<f32>, b: vec3<f32>", "pow(a, b)"},
      {"mix_wrong_arity", "a: f32", "mix(a, a)"},
      {"mix_wrong_weight", "a: vec2<f32>, b: vec3<f32>", "mix(a, a, b)"},
      {"all_integer", "a: vec2<i32>", "all(a)"},
      {"clamp_wrong_width", "a: vec2<f32>, b: vec3<f32>", "clamp(a, a, b)"},
      {"clamp_wrong_arity", "a: f32", "clamp(a, a)"},
      {"select_integer_condition", "a: f32, b: i32", "select(a, a, b)"},
      {"select_condition_width", "a: vec2<f32>, b: vec3<bool>", "select(a, a, b)"},
      {"min_boolean", "a: bool", "min(a, a)"},
      {"max_wrong_type", "a: f32, b: i32", "max(a, b)"},
      {"sqrt_integer", "a: i32", "sqrt(a)"},
      {"sin_wrong_arity", "a: f32", "sin(a, a)"},
      {"texture_dimensions_value", "a: f32", "textureDimensions(a)"},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.name);
    const std::string source =
        std::string("fn f(") + item.parameters + ") { let result = " + item.call + "; }";
    EXPECT_THAT(source, RejectedWithCode(ErrorCode::InvalidCall));
  }
}

// The arena-limit tests below build their sources from the ModuleLimits constants so they keep
// tracking the limits. Each construct is chosen so the intended limit trips before any other:
// parentheses add tokens without arena entries, module constants add symbols without statements,
// and repeated assignments add statements while staying well under the expression budget.

TEST(Diagnostics, TokenLimitTripsBeforeOtherArenas) {
  // Each statement costs 20 tokens, 2 expressions and 1 statement, so the token budget is the
  // first to run out (kMaxTokens / 20 statements is far below kMaxStatements).
  constexpr std::string_view kStatement = "v = ((((((((0i))))))));\n";
  constexpr uint32_t kTokensPerStatement = 20;
  std::string source = "fn f() { var v: i32 = 0i;\n";
  const uint32_t statements = ModuleLimits::kMaxTokens / kTokensPerStatement + 2;
  static_assert(ModuleLimits::kMaxTokens / kTokensPerStatement + 2 < ModuleLimits::kMaxStatements);
  for (uint32_t i = 0; i < statements; ++i) {
    source += kStatement;
  }
  source += "}\n";
  ExpectRejected(source, ErrorCode::TokenLimit);
}

TEST(Diagnostics, IdentifierLimitTripsOnTheSecondLongName) {
  // The parser records a function name twice (the function and its symbol), so the first name
  // fills the identifier arena exactly and the second declaration trips the limit.
  const std::string first(ModuleLimits::kMaxIdentifierBytes / 2, 'a');
  const std::string second(ModuleLimits::kMaxIdentifierBytes / 2 + 1, 'b');
  const std::string source = "fn " + first + "() {}\nfn " + second + "() {}\n";
  ExpectRejected(source, ErrorCode::IdentifierLimit);
}

TEST(Diagnostics, StructLimitTripsOnOneStructPastTheArena) {
  std::string source;
  for (uint32_t i = 0; i <= ModuleLimits::kMaxStructs; ++i) {
    source += "struct S" + std::to_string(i) + " { value: f32, }\n";
  }
  ExpectRejected(source, ErrorCode::StructLimit);
}

TEST(Diagnostics, StructMemberLimitTripsOnOneMemberPastTheArena) {
  std::string source = "struct S {\n";
  for (uint32_t i = 0; i <= ModuleLimits::kMaxStructMembers; ++i) {
    source += "  m" + std::to_string(i) + ": f32,\n";
  }
  source += "}\n";
  ExpectRejected(source, ErrorCode::StructMemberLimit);
}

TEST(Diagnostics, BindingLimitTripsOnOneBindingPastTheArena) {
  std::string source;
  for (uint32_t i = 0; i <= ModuleLimits::kMaxBindings; ++i) {
    source += "@group(0) @binding(" + std::to_string(i) + ") var t" + std::to_string(i) +
              ": texture_2d<f32>;\n";
  }
  ExpectRejected(source, ErrorCode::BindingLimit);
}

TEST(Diagnostics, BindingIndexIsBoundedByTheRuntimeBindingCap) {
  // Reflection turns every accepted binding into a bind group layout entry, and the runtime
  // refuses an entry whose index reaches kMaxBindings. A higher index accepted here would compile
  // to an artifact no device can bind.
  EXPECT_THAT(TextureBindingSource(gpu::kMaxBindings - 1), ParsedSuccessfully());
  EXPECT_THAT(TextureBindingSource(gpu::kMaxBindings), RejectedWithCode(ErrorCode::InvalidBinding));
}

TEST(Diagnostics, WorkgroupSizeIsBoundedByTheRuntimeComputeCaps) {
  // A declared workgroup size reaches compute pipeline creation verbatim, which caps the Z extent
  // well below the X and Y extents and caps the product below either.
  static_assert(gpu::kMaxComputeWorkgroupSizeZ < gpu::kMaxComputeWorkgroupSizeXY,
                "The Z case below is what distinguishes the Z cap from the X and Y caps");

  EXPECT_THAT(WorkgroupSource(1, 1, gpu::kMaxComputeWorkgroupSizeZ), ParsedSuccessfully());
  EXPECT_THAT(WorkgroupSource(1, 1, gpu::kMaxComputeWorkgroupSizeZ + 1),
              RejectedWithCode(ErrorCode::InvalidAttribute));

  EXPECT_THAT(WorkgroupSource(gpu::kMaxComputeWorkgroupSizeXY, 1, 1), ParsedSuccessfully());
  EXPECT_THAT(WorkgroupSource(gpu::kMaxComputeWorkgroupSizeXY + 1, 1, 1),
              RejectedWithCode(ErrorCode::InvalidAttribute));
  EXPECT_THAT(WorkgroupSource(1, gpu::kMaxComputeWorkgroupSizeXY, 1), ParsedSuccessfully());
  EXPECT_THAT(WorkgroupSource(1, gpu::kMaxComputeWorkgroupSizeXY + 1, 1),
              RejectedWithCode(ErrorCode::InvalidAttribute));

  // Each extent below its own cap, and their product above the invocation cap.
  static_assert(16 * 16 * 2 > gpu::kMaxComputeInvocationsPerWorkgroup);
  EXPECT_THAT(WorkgroupSource(16, 16, 1), ParsedSuccessfully());
  EXPECT_THAT(WorkgroupSource(16, 16, 2), RejectedWithCode(ErrorCode::InvalidAttribute));
}

TEST(Diagnostics, SymbolLimitTripsOnModuleConstantsWithoutStatements) {
  // Module-level constants consume a symbol and one literal expression each, but no statement,
  // so the symbol arena runs out first.
  std::string source;
  for (uint32_t i = 0; i <= ModuleLimits::kMaxSymbols; ++i) {
    source += "const c" + std::to_string(i) + " = 1i;\n";
  }
  ExpectRejected(source, ErrorCode::SymbolLimit);
}

TEST(Diagnostics, ExpressionLimitTripsBeforeStatementsAndSymbols) {
  // Nine operands and eight additions cost 17 expressions per statement at depth nine, which is
  // under kMaxNesting; the expression arena runs out after about a quarter of kMaxStatements.
  constexpr uint32_t kExpressionsPerStatement = 17;
  const uint32_t statements = ModuleLimits::kMaxExpressions / kExpressionsPerStatement + 2;
  static_assert(ModuleLimits::kMaxExpressions / kExpressionsPerStatement + 2 <
                ModuleLimits::kMaxStatements);
  std::string source = "fn f(x: i32) {\n";
  for (uint32_t i = 0; i < statements; ++i) {
    source += "let v" + std::to_string(i) + " = x + x + x + x + x + x + x + x + x;\n";
  }
  source += "}\n";
  ExpectRejected(source, ErrorCode::ExpressionLimit);
}

TEST(Diagnostics, StatementLimitTripsOnRepeatedAssignments) {
  // Each assignment costs one statement and two expressions and declares nothing.
  std::string source = "fn f() { var v: i32 = 0i;\n";
  for (uint32_t i = 0; i <= ModuleLimits::kMaxStatements; ++i) {
    source += "v = 1i;\n";
  }
  source += "}\n";
  ExpectRejected(source, ErrorCode::StatementLimit);
}

TEST(Diagnostics, FunctionLimitTripsOnOneFunctionPastTheArena) {
  std::string source;
  for (uint32_t i = 0; i <= ModuleLimits::kMaxFunctions; ++i) {
    source += "fn helper" + std::to_string(i) + "() {}\n";
  }
  ExpectRejected(source, ErrorCode::FunctionLimit);
}

TEST(Diagnostics, UniformityLimitTripsWhenLoopPhisExhaustTheDependencyGraph) {
  // The uniformity analyzer allocates one graph node per mutable local for every loop it enters.
  // Many locals followed by many loops exhaust the graph's node capacity while every parser
  // arena stays well within budget. The analyzer only runs for modules that contain a collective
  // operation, so a fragment entry point with a textureSample call follows the helper.
  constexpr uint32_t kLocals = 500;
  constexpr uint32_t kLoops = 20;
  std::string source =
      "@group(0) @binding(0) var t: texture_2d<f32>;\n"
      "@group(0) @binding(1) var s: sampler;\n"
      "fn f() {\n";
  for (uint32_t i = 0; i < kLocals; ++i) {
    source += "var v" + std::to_string(i) + ": i32 = 0i;\n";
  }
  for (uint32_t i = 0; i < kLoops; ++i) {
    source += "for (var i" + std::to_string(i) + ": i32 = 0i; i" + std::to_string(i) + " < 4i; i" +
              std::to_string(i) + " = i" + std::to_string(i) + " + 1i) {}\n";
  }
  source +=
      "}\n"
      "@fragment fn fs_main(@builtin(position) p: vec4f) -> @location(0) vec4f {\n"
      "  return textureSample(t, s, vec2f(0.5));\n"
      "}\n";
  ExpectRejected(source, ErrorCode::UniformityLimit);
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
