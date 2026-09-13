#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>

#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/tests/ControlSource.h"
#include "donner/gpu/shader/wgsl/tests/GraphicsArtifact.h"

namespace donner::gpu::shader::wgsl {
namespace {

TEST(Language, MaterializesAbstractConstantsAndPreservesExplicitTypes) {
  constexpr auto result = Parse(R"(
const noBand: u32 = 0xFFFFFFFFu;
const scale = 1.0 / 65536.0;
fn scalar(x: f32) -> f32 { let offset = 0; let next = offset + -1; return x * scale; }
)");
  static_assert(result.hasResult());
  ASSERT_EQ(result.diagnostic.code, ErrorCode::None);
  EXPECT_EQ(result.module.symbols[0].type, (Type{TypeKind::U32}));
  EXPECT_EQ(result.module.expressions[result.module.symbols[0].constantExpression].payload,
            0xffffffffu);
  EXPECT_EQ(result.module.symbols[1].type, (Type{TypeKind::AbstractFloat}));
}

TEST(Language, RejectsMalformedConstantDeclarationsWithoutInvalidArenaAccess) {
  struct Case {
    const char* source;
    ErrorCode error;
  };
  for (const auto& item :
       {Case{"fn helper() -> i32 { return 1; } const X = helper();",
             ErrorCode::InvalidConstantExpression},
        Case{"const X: f32 = 1i;", ErrorCode::TypeMismatch},
        Case{"const X: u32 = -1;", ErrorCode::InvalidConstantExpression},
        Case{"const X = 9223372036854775807 + 1;", ErrorCode::InvalidConstantExpression},
        Case{"const X = 1.0 / 0.0;", ErrorCode::InvalidConstantExpression},
        Case{"fn f(x: i32) { for(var i=0; i<x; i=i+(-1)) {} }", ErrorCode::InvalidLoop},
        Case{"fn f() { break; }", ErrorCode::InvalidLoop},
        Case{"fn f() { continue; }", ErrorCode::InvalidLoop},
        Case{"fn f() { discard; }", ErrorCode::UnsupportedConstruct},
        Case{"@group(0) @binding(16) var s: sampler;", ErrorCode::InvalidBinding}}) {
    SCOPED_TRACE(item.source);
    EXPECT_EQ(Parse(item.source).diagnostic.code, item.error);
  }
}

TEST(Language, EnforcesWgslOperatorGrouping) {
  for (const char* source : {"fn f(a:bool,b:bool,c:bool)->bool { return a || b && c; }",
                             "fn f(a:bool,b:bool,c:bool)->bool { return a && b || c; }",
                             "fn f(a:bool,b:bool,c:bool)->bool { return a == b == c; }",
                             "fn f(a:u32,b:u32,c:u32)->u32 { return a & b + c; }"}) {
    SCOPED_TRACE(source);
    EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::UnsupportedConstruct);
  }
  for (const char* source : {"fn f(a:bool,b:bool,c:bool)->bool { return a || (b && c); }",
                             "fn f(a:bool,b:bool,c:bool)->bool { return a == (b == c); }",
                             "fn f(a:u32,b:u32,c:u32)->u32 { return a & (b + c); }"}) {
    SCOPED_TRACE(source);
    EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::None);
  }
}

TEST(Language, RequiresDerivativesInStraightLineFragmentEntryCode) {
  for (const char* body :
       {"let b = (p.x > 0.0) && (fwidth(p.x) > 0.0);",
        "let b = (p.x > 0.0) || (fwidth(p.x) > 0.0);", "if (p.x > 0.0) { let w=fwidth(p.x); }",
        "if (p.x > 0.0) { discard; } let w=fwidth(p.x);"}) {
    const std::string source =
        std::string("@fragment fn f(@builtin(position) p:vec4f)->@location(0) vec4f {") + body +
        "return vec4f(0.0); }";
    SCOPED_TRACE(source);
    EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::UnsupportedConstruct);
  }
  EXPECT_EQ(Parse("fn f(x:f32)->f32 { return fwidth(x); }").diagnostic.code,
            ErrorCode::UnsupportedConstruct);
}

TEST(Language, DiscardDoesNotReplaceTheAuthoredReturn) {
  EXPECT_EQ(Parse("@fragment fn f()->@location(0) f32 { discard; }").diagnostic.code,
            ErrorCode::MissingReturn);
  EXPECT_EQ(Parse("@fragment fn f()->@location(0) f32 { discard; return 0.0; }").diagnostic.code,
            ErrorCode::None);
  EXPECT_EQ(Parse("@fragment fn f(@builtin(position) p:vec4f)->@location(0) f32 { discard; return "
                  "fwidth(p.x); }")
                .diagnostic.code,
            ErrorCode::UnsupportedConstruct);
}

TEST(Language, BoundsElseIfChains) {
  std::string source = "fn f(b: bool) { if(b) {}";
  for (uint16_t i = 0; i < ModuleLimits::kMaxNesting; ++i) source += " else if(b) {}";
  source += " }";
  EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::NestingLimit);
}

TEST(Language, ReflectsOnlyStaticallyUsedResourcesInTheControlFixture) {
  const auto& shader = tests::ControlShader();
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().stage, ShaderStage::Fragment);
  EXPECT_EQ(shader.entryPoints.front().resourceMask, 1u);
  ASSERT_THAT(shader.resources, testing::SizeIs(2));
  EXPECT_EQ(shader.resources[0].runtimeArrayStrideBytes, 4u);
  EXPECT_EQ(shader.resources[1].type, BindingType::FilteringSampler);
  EXPECT_THAT(MakeBindingLayout(shader), testing::SizeIs(1));
  EXPECT_THAT(shader.msl, testing::Not(testing::HasSubstr("[[sampler(4)]]")));
}

TEST(Language, OrdinaryAndFrozenControlProjectionsAgree) {
  const auto parsed = Parse(tests::kControlSource.view());
  ASSERT_EQ(parsed.diagnostic.code, ErrorCode::None);
  const auto& frozen = tests::ControlShader();
  std::array<char, 16384> msl{};
  TextSink text{msl.data(), uint32_t(msl.size())};
  ASSERT_EQ(EmitMsl(parsed.module, text).error, TextEmitError::None);
  EXPECT_EQ(text.view(), frozen.msl);
  std::array<uint32_t, 4096> words{};
  SpirvSink binary{words.data(), uint32_t(words.size())};
  ASSERT_EQ(EmitSpirv(parsed.module, binary).error, SpirvEmitError::None);
  EXPECT_THAT(std::span(words.data(), binary.size), testing::ElementsAreArray(frozen.spirv));
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
