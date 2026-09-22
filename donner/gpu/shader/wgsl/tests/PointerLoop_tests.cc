#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/SpirvEmitter.h"
#include "donner/gpu/shader/wgsl/TextEmitter.h"

namespace donner::gpu::shader::wgsl {
namespace {

/// Names the diagnostic categories these cases assert so a failure reads without the enum value.
constexpr std::string_view ErrorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::None: return "None";
    case ErrorCode::NestingLimit: return "NestingLimit";
    case ErrorCode::UnexpectedToken: return "UnexpectedToken";
    case ErrorCode::UnsupportedConstruct: return "UnsupportedConstruct";
    case ErrorCode::TypeMismatch: return "TypeMismatch";
    case ErrorCode::InvalidLayout: return "InvalidLayout";
    case ErrorCode::ImmutableAssignment: return "ImmutableAssignment";
    case ErrorCode::InvalidCall: return "InvalidCall";
    case ErrorCode::InvalidCondition: return "InvalidCondition";
    case ErrorCode::InvalidLoop: return "InvalidLoop";
    case ErrorCode::InvalidPointer: return "InvalidPointer";
    default: return "other";
  }
}

void PrintTo(const ErrorCode& code, std::ostream* out) {
  *out << ErrorCodeName(code) << '(' << static_cast<int>(code) << ')';
}

/// A diagnostic paired with the source text its span covers, so a mismatch names the construct.
struct Rejection {
  ErrorCode code = ErrorCode::None;
  std::string spanned;

  bool operator==(const Rejection&) const = default;
};

void PrintTo(const Rejection& rejection, std::ostream* out) {
  PrintTo(rejection.code, out);
  *out << " over \"" << rejection.spanned << '"';
}

/// Parses p source and reports its diagnostic together with the bytes the span selects.
/// @param source WGSL to reject.
Rejection Reject(std::string_view source) {
  const ParseResult parsed = Parse(source);
  const uint32_t limit = static_cast<uint32_t>(source.size());
  const uint32_t end = parsed.diagnostic.span.end < limit ? parsed.diagnostic.span.end : limit;
  const uint32_t begin = parsed.diagnostic.span.begin < end ? parsed.diagnostic.span.begin : end;
  return Rejection{parsed.diagnostic.code, std::string(source.substr(begin, end - begin))};
}

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

constexpr std::string_view kResources =
    "@group(0) @binding(0) var inputTexture:texture_2d<f32>;\n"
    "@group(0) @binding(1) var outputTexture:texture_storage_2d<rgba32float,write>;\n";

constexpr std::string_view kEntry =
    "@compute @workgroup_size(1) fn cs_main(@builtin(global_invocation_id) gid:vec3u){\n"
    "  textureStore(outputTexture,vec2i(gid.xy),vec4f(evaluate(0.5)));\n}\n";

/// Wraps a helper body in the shared texture resources and compute entry.
/// @param body Helper declarations ending with `fn evaluate(seed: f32) -> f32`.
std::string Program(std::string_view body) {
  return std::string(kResources) + std::string(body) + std::string(kEntry);
}

constexpr std::string_view kPointerLoopBody = R"(
const kSlots: u32 = 4u;

struct Accumulator {
  values: array<vec2f, kSlots>,
  count: u32,
  total: i32,
  filled: bool,
};

struct Summary { sum: f32, steps: i32, complete: bool, };

fn push(state: ptr<function, Accumulator>, value: f32) {
  if ((*state).count == kSlots) {
    (*state).filled = false;
    return;
  }
  (*state).values[(*state).count] = vec2f(value, 1.0);
  (*state).count += 1u;
  (*state).total += 1;
}

fn summarize(state: ptr<function, Accumulator>) -> Summary {
  var sum = 0.0;
  var steps = 0;
  var index = 0u;
  while (index < (*state).count) {
    sum += (*state).values[index].x;
    steps += 1;
    index += 1u;
  }
  return Summary(sum, steps, (*state).filled);
}

fn evaluate(seed: f32) -> f32 {
  var state: Accumulator;
  state.filled = true;
  var step = 0u;
  loop {
    if (step == kSlots) {
      break;
    }
    push(&state, seed * f32(step + 1u));
    step += 1u;
  }
  push(&state, seed);
  let summary = summarize(&state);
  var result = summary.sum;
  result -= seed;
  result += select(0.0, 0.5, !summary.complete);
  return result + f32(summary.steps + state.total) * 0.0625;
}
)";

TEST(PointerLoop, AcceptsPointerParametersLoopsAndCompoundAssignments) {
  const std::string source = Program(kPointerLoopBody);
  const ParseResult parsed = Parse(source);
  ASSERT_THAT(Reject(source), testing::Eq(Rejection{ErrorCode::None, ""}));
  ASSERT_THAT(parsed.hasResult(), testing::IsTrue());

  const Type pointer = TypeOfSymbol(parsed.module, "state");
  EXPECT_THAT(pointer.kind, testing::Eq(TypeKind::Pointer));
  EXPECT_THAT(pointer.elementType().kind, testing::Eq(TypeKind::Struct));

  // Abstract initializers concretize to i32, u32 and f32 without a declared type.
  EXPECT_THAT(TypeOfSymbol(parsed.module, "steps"), testing::Eq(Type{TypeKind::I32}));
  EXPECT_THAT(TypeOfSymbol(parsed.module, "index"), testing::Eq(Type{TypeKind::U32}));
  EXPECT_THAT(TypeOfSymbol(parsed.module, "sum"), testing::Eq(Type{TypeKind::F32}));

  // A structure reaching a bool is function-scope only.
  EXPECT_THAT(parsed.module.structs[0].hostShareable, testing::IsFalse());
  EXPECT_THAT(parsed.module.structs[1].hostShareable, testing::IsFalse());
}

TEST(PointerLoop, ProjectsPointerParametersAndLoopsToBothBackends) {
  const std::string source = Program(kPointerLoopBody);
  const ParseResult parsed = Parse(source);
  ASSERT_THAT(parsed.hasResult(), testing::IsTrue());

  std::string msl(kMaxTextEmitBytes, '\0');
  TextSink textSink{msl.data(), static_cast<uint32_t>(msl.size())};
  ASSERT_THAT(EmitMsl(parsed.module, textSink).error, testing::Eq(TextEmitError::None));
  const std::string_view emitted = textSink.view();
  EXPECT_THAT(emitted, testing::HasSubstr("thread donner_msl_struct_Accumulator&"));
  EXPECT_THAT(emitted, testing::HasSubstr("for (;;) {"));
  EXPECT_THAT(emitted, testing::HasSubstr("while ("));

  std::vector<uint32_t> words(24576);
  SpirvSink spirvSink{words.data(), static_cast<uint32_t>(words.size())};
  EXPECT_THAT(EmitSpirv(parsed.module, spirvSink).error, testing::Eq(SpirvEmitError::None));
  EXPECT_THAT(spirvSink.size, testing::Gt(0u));
}

/// Parses p body and emits both projections, returning the first failure it hits.
/// @param body Helper declarations ending with `fn evaluate(seed: f32) -> f32`.
Rejection Accept(std::string_view body) {
  const std::string source = Program(body);
  const Rejection rejection = Reject(source);
  if (rejection.code != ErrorCode::None) {
    return rejection;
  }
  const ParseResult parsed = Parse(source);
  std::string msl(kMaxTextEmitBytes, '\0');
  TextSink textSink{msl.data(), static_cast<uint32_t>(msl.size())};
  if (!EmitMsl(parsed.module, textSink).ok()) {
    return Rejection{ErrorCode::UnsupportedConstruct, "msl"};
  }
  std::vector<uint32_t> words(24576);
  SpirvSink spirvSink{words.data(), static_cast<uint32_t>(words.size())};
  if (EmitSpirv(parsed.module, spirvSink).error != SpirvEmitError::None) {
    return Rejection{ErrorCode::UnsupportedConstruct, "spirv"};
  }
  return rejection;
}

TEST(PointerLoop, AcceptsLoopContinueAndIndirectPointerWrites) {
  const Rejection accepted{ErrorCode::None, ""};
  EXPECT_THAT(Accept("fn evaluate(seed: f32) -> f32 {\n"
                     "  var total = 0.0;\n  var i = 0u;\n"
                     "  loop { i += 1u; if (i == 6u) { break; }\n"
                     "         if (i == 3u) { continue; }\n  total += seed; }\n"
                     "  return total;\n}\n"),
              testing::Eq(accepted));
  EXPECT_THAT(Accept("fn evaluate(seed: f32) -> f32 {\n"
                     "  var total = 0.0;\n  var i = 0u;\n"
                     "  while (i < 5u) { i += 1u; if (i == 3u) { continue; } total += seed; }\n"
                     "  return total;\n}\n"),
              testing::Eq(accepted));
  // A body that always breaks still needs a well-formed continue target.
  EXPECT_THAT(Accept("fn evaluate(seed: f32) -> f32 {\n"
                     "  var total = seed;\n  loop { total += 1.0; break; }\n  return total;\n}\n"),
              testing::Eq(accepted));
  EXPECT_THAT(Accept("fn evaluate(seed: f32) -> f32 {\n"
                     "  var i = 0u;\n  while (i < 5u) { return seed; }\n  return 0.0;\n}\n"),
              testing::Eq(accepted));
  EXPECT_THAT(Accept("struct Box { value: f32, pair: vec2f, flag: bool, };\n"
                     "fn reset(box: ptr<function, Box>, seed: f32) {\n"
                     "  *box = Box(seed, vec2f(seed, seed), true);\n"
                     "  (*box).pair += vec2f(1.0, 2.0);\n"
                     "  (*box).pair.x -= 0.5;\n}\n"
                     "fn evaluate(seed: f32) -> f32 {\n"
                     "  var box: Box;\n  reset(&box, seed);\n"
                     "  return box.value + box.pair.x + box.pair.y;\n}\n"),
              testing::Eq(accepted));
  // A pointer parameter may be forwarded to another pointer parameter.
  EXPECT_THAT(Accept("struct Box { value: f32, flag: bool, };\n"
                     "fn bump(box: ptr<function, Box>) { (*box).value += 1.0; }\n"
                     "fn forward(box: ptr<function, Box>) { bump(box); bump(box); }\n"
                     "fn evaluate(seed: f32) -> f32 {\n"
                     "  var box: Box;\n  box.value = seed;\n  forward(&box);\n"
                     "  return box.value;\n}\n"),
              testing::Eq(accepted));
  EXPECT_THAT(Accept("fn bump(total: ptr<function, f32>, x: f32) { *total += x; }\n"
                     "fn evaluate(seed: f32) -> f32 {\n"
                     "  var total = 0.0;\n  bump(&total, seed);\n  bump(&total, seed);\n"
                     "  return total;\n}\n"),
              testing::Eq(accepted));
}

TEST(PointerLoop, RejectsPointersOutsideTheFunctionAddressSpace) {
  constexpr std::string_view kBox = "struct Box { value: f32, flag: bool, };\n";
  const auto reject = [&](std::string_view declaration) {
    return Reject(std::string(kResources) + std::string(kBox) + std::string(declaration));
  };
  EXPECT_THAT(reject("fn f(p: ptr<storage, Box>) -> f32 { return (*p).value; }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "storage"}));
  EXPECT_THAT(reject("fn f(p: ptr<uniform, Box>) -> f32 { return (*p).value; }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "uniform"}));
  EXPECT_THAT(reject("fn f(p: ptr<private, Box>) -> f32 { return (*p).value; }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "private"}));
  EXPECT_THAT(reject("fn f(p: ptr<function, Box, read>) -> f32 { return (*p).value; }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "read"}));
  EXPECT_THAT(Reject(std::string(kResources) +
                     "fn f(p: ptr<function, array<f32,4>>) -> f32 { return 0.0; }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "ptr"}));
}

TEST(PointerLoop, RejectsAddressesThatAreNotWholeLocalVariables) {
  constexpr std::string_view kBox = "struct Box { value: f32, flag: bool, };\n";
  constexpr std::string_view kTaker = "fn g(p: ptr<function, Box>) -> f32 { return (*p).value; }\n";
  const auto reject = [&](std::string_view body) {
    return Reject(std::string(kResources) + std::string(kBox) + std::string(kTaker) +
                  std::string(body));
  };
  EXPECT_THAT(reject("fn f(box: Box) -> f32 { return g(&box); }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "&"}));
  EXPECT_THAT(reject("fn f() -> f32 { let box = Box(1.0, true); return g(&box); }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "&"}));
  EXPECT_THAT(reject("struct Pair { a: Box, };\n"
                     "fn f() -> f32 { var pair: Pair; return g(&pair.a); }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "&"}));
  EXPECT_THAT(
      Reject(std::string(kResources) + "fn g(p: ptr<function, f32>) -> f32 { return *p; }\n"
                                       "fn f() -> f32 { var a: array<f32,4>; return g(&a[1]); }"),
      testing::Eq(Rejection{ErrorCode::InvalidPointer, "&"}));
  EXPECT_THAT(Reject(std::string(kResources) + "struct P { v: f32, };\n" +
                     "@group(0) @binding(2) var<uniform> params: P;\n"
                     "fn g(p: ptr<function, P>) -> f32 { return (*p).v; }\n"
                     "fn f() -> f32 { return g(&params); }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "&"}));
}

TEST(PointerLoop, RejectsStoringReturningAndEmbeddingPointers) {
  constexpr std::string_view kBox = "struct Box { value: f32, flag: bool, };\n";
  const auto reject = [&](std::string_view declaration) {
    return Reject(std::string(kResources) + std::string(kBox) + std::string(declaration));
  };
  EXPECT_THAT(reject("fn g(p: ptr<function, Box>) -> f32 { let q = p; return (*q).value; }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "q"}));
  EXPECT_THAT(reject("fn g(p: ptr<function, Box>) -> f32 {\n"
                     "  var q: ptr<function, Box>;\n  return (*p).value;\n}"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "q"}));
  EXPECT_THAT(reject("fn g(p: ptr<function, Box>) -> ptr<function, Box> { return p; }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "g"}));
  EXPECT_THAT(reject("struct Holder { p: ptr<function, Box>, };"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "p"}));
  EXPECT_THAT(reject("@compute @workgroup_size(1) fn cs(p: ptr<function, Box>) {}"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "p"}));
  EXPECT_THAT(Reject(std::string(kResources) + "fn f(x: f32) -> f32 { return *x; }"),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "*"}));
}

TEST(PointerLoop, RejectsTwoPointerArgumentsAddressingOneVariable) {
  const auto source = [&](std::string_view call) {
    return std::string(kResources) +
           "fn pair(a: ptr<function, f32>, b: ptr<function, f32>) -> f32 {\n"
           "  *a += 1.0;\n  return *b;\n}\n"
           "fn evaluate(seed: f32) -> f32 {\n  var x = seed;\n  var y = seed;\n  return " +
           std::string(call) + ";\n}\n" + std::string(kEntry);
  };
  EXPECT_THAT(Reject(source("pair(&x, &x)")),
              testing::Eq(Rejection{ErrorCode::InvalidPointer, "pair"}));
  EXPECT_THAT(Reject(source("pair(&x, &y)")), testing::Eq(Rejection{ErrorCode::None, ""}));
}

TEST(PointerLoop, KeepsGraphAllocationOffCallsWithoutPointerArguments) {
  // Every call used to allocate dependency nodes whether or not a pointer escaped through it.
  std::string source = std::string(kResources) +
                       "fn add2(a: f32, b: f32) -> f32 { return a + b; }\n"
                       "@fragment fn fs_main(@builtin(position) p: vec4f) -> @location(0) vec4f {\n"
                       "  let d = fwidth(p.x);\n  var total = 0.0;\n";
  for (unsigned call = 0; call < 450; ++call) {
    source += "  total += add2(p.x, p.y);\n";
  }
  source += "  return vec4f(total + d);\n}\n";
  EXPECT_THAT(Reject(source), testing::Eq(Rejection{ErrorCode::None, ""}));
}

TEST(PointerLoop, RejectsPointerAndValueArgumentSubstitution) {
  constexpr std::string_view kBox = "struct Box { value: f32, flag: bool, };\n";
  const auto reject = [&](std::string_view declaration) {
    return Reject(std::string(kResources) + std::string(kBox) + std::string(declaration));
  };
  EXPECT_THAT(reject("fn g(p: ptr<function, Box>) -> f32 { return (*p).value; }\n"
                     "fn f() -> f32 { var box: Box; return g(box); }"),
              testing::Eq(Rejection{ErrorCode::InvalidCall, "g"}));
  EXPECT_THAT(reject("fn g(b: Box) -> f32 { return b.value; }\n"
                     "fn f() -> f32 { var box: Box; return g(&box); }"),
              testing::Eq(Rejection{ErrorCode::InvalidCall, "g"}));
}

TEST(PointerLoop, RejectsBoolMembersInHostShareableBuffers) {
  constexpr std::string_view kBox = "struct Box { value: f32, flag: bool, };\n";
  const auto reject = [&](std::string_view declaration) {
    return Reject(std::string(kResources) + std::string(kBox) + std::string(declaration));
  };
  EXPECT_THAT(reject("@group(0) @binding(2) var<uniform> params: Box;"),
              testing::Eq(Rejection{ErrorCode::InvalidLayout, "params"}));
  EXPECT_THAT(reject("@group(0) @binding(2) var<storage,read> records: array<Box>;"),
              testing::Eq(Rejection{ErrorCode::InvalidLayout, "records"}));
  EXPECT_THAT(reject("struct Outer { box: Box, };\n"
                     "@group(0) @binding(2) var<uniform> params: Outer;"),
              testing::Eq(Rejection{ErrorCode::InvalidLayout, "params"}));
}

TEST(PointerLoop, RejectsMalformedLoopsAndLoopControl) {
  const auto reject = [&](std::string_view declaration) {
    return Reject(std::string(kResources) + std::string(declaration));
  };
  EXPECT_THAT(reject("fn f() -> f32 { break; }"),
              testing::Eq(Rejection{ErrorCode::InvalidLoop, "break"}));
  EXPECT_THAT(reject("fn f() -> f32 { continue; }"),
              testing::Eq(Rejection{ErrorCode::InvalidLoop, "continue"}));
  EXPECT_THAT(reject("fn f() -> f32 { var i = 0u; loop { i += 1u; } return f32(i); }"),
              testing::Eq(Rejection{ErrorCode::InvalidLoop, "loop"}));
  EXPECT_THAT(reject("fn f() -> f32 { var i = 0u;\n"
                     "  loop { i += 1u; if (i == 2u) { break; } continuing { i += 1u; } }\n"
                     "  return f32(i); }"),
              testing::Eq(Rejection{ErrorCode::InvalidLoop, "continuing"}));
  EXPECT_THAT(reject("fn f() -> f32 { var i = 0u; while (i) { i += 1u; } return f32(i); }"),
              testing::Eq(Rejection{ErrorCode::InvalidCondition, "i"}));
}

TEST(PointerLoop, BoundsLoopNesting) {
  std::string body = "fn f() -> f32 { var i = 0u;";
  for (unsigned depth = 0; depth < ModuleLimits::kMaxLoopDepth; ++depth) {
    body += "loop { if (i > " + std::to_string(depth) + "u) { break; }";
  }
  std::string closing;
  for (unsigned depth = 0; depth < ModuleLimits::kMaxLoopDepth; ++depth) {
    closing += "}";
  }
  EXPECT_THAT(Reject(std::string(kResources) + body + " i += 1u;" + closing + " return f32(i); }"),
              testing::Eq(Rejection{ErrorCode::None, ""}));

  body += "loop { if (i > 99u) { break; }";
  EXPECT_THAT(Reject(std::string(kResources) + body + " i += 1u;" + closing + "} return f32(i); }"),
              testing::Eq(Rejection{ErrorCode::NestingLimit, "loop"}));
}

TEST(PointerLoop, RejectsCompoundAssignmentToImmutableOrRepeatedTargets) {
  const auto reject = [&](std::string_view declaration) {
    return Reject(std::string(kResources) + std::string(declaration));
  };
  EXPECT_THAT(reject("fn f() -> f32 { let x = 1.0; x += 1.0; return x; }"),
              testing::Eq(Rejection{ErrorCode::ImmutableAssignment, "x"}));
  EXPECT_THAT(reject("fn f(x: f32) -> f32 { x += 1.0; return x; }"),
              testing::Eq(Rejection{ErrorCode::ImmutableAssignment, "x"}));
  // The target is read and written, so a call inside it would run twice.
  EXPECT_THAT(reject("fn h(i: u32) -> u32 { return i; }\n"
                     "fn f() -> f32 { var a: array<f32,4>; a[h(0u)] += 1.0; return a[0]; }"),
              testing::Eq(Rejection{ErrorCode::UnsupportedConstruct, "a[h(0u)]"}));
}

TEST(PointerLoop, RejectsExpressionStatementsThatAreNotVoidCalls) {
  const auto reject = [&](std::string_view declaration) {
    return Reject(std::string(kResources) + std::string(declaration));
  };
  EXPECT_THAT(reject("fn f() -> f32 { var x = 1.0; x; return x; }"),
              testing::Eq(Rejection{ErrorCode::InvalidCall, "x"}));
  EXPECT_THAT(reject("fn h() -> f32 { return 1.0; }\nfn f() -> f32 { h(); return 0.0; }"),
              testing::Eq(Rejection{ErrorCode::InvalidCall, "h()"}));
}

TEST(PointerLoop, WrapsIntegerCompoundAssignmentLikeTheSpelledOutForm) {
  const auto module = [&](std::string_view body) {
    const ParseResult parsed =
        Parse(std::string(kResources) + std::string(body) + "\n" + std::string(kEntry));
    std::string msl(kMaxTextEmitBytes, '\0');
    TextSink sink{msl.data(), static_cast<uint32_t>(msl.size())};
    const TextEmitResult result = EmitMsl(parsed.module, sink);
    return std::string(result.ok() ? sink.view() : std::string_view());
  };
  const std::string compound =
      module("fn evaluate(seed: f32) -> f32 { var i = 0; i += 3; return f32(i); }");
  const std::string spelled =
      module("fn evaluate(seed: f32) -> f32 { var i = 0; i = i + 3; return f32(i); }");
  EXPECT_THAT(compound, testing::Not(testing::IsEmpty()));
  EXPECT_THAT(compound, testing::Eq(spelled));
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
