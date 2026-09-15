#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

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

TEST(Diagnostics, ReportsExactCodeForMinimalSemanticViolations) {
  for (const DiagnosticCase& item : kSemanticCases) {
    SCOPED_TRACE(item.name);
    ExpectRejected(item.source, item.expected);
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
  for (uint32_t i = 0; i < statements; ++i) source += kStatement;
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
  for (uint32_t i = 0; i <= ModuleLimits::kMaxStatements; ++i) source += "v = 1i;\n";
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
