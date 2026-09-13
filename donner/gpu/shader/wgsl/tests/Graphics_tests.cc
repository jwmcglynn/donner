#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>

#include "donner/gpu/shader/wgsl/tests/GraphicsArtifact.h"
#include "donner/gpu/shader/wgsl/tests/GraphicsSource.h"
#include "donner/gpu/shader/wgsl/tests/MatrixSource.h"

namespace donner::gpu::shader::wgsl {
namespace {
using testing::SizeIs;

TEST(GraphicsCompiler, ReflectsBothStagesAndTheirInterfaces) {
  const auto& shader = tests::GraphicsShader();
  ASSERT_THAT(shader.entryPoints, SizeIs(2));
  const auto& vertex = shader.entryPoints[0];
  const auto& fragment = shader.entryPoints[1];
  EXPECT_EQ(vertex.name.view(), "vs_main");
  EXPECT_EQ(vertex.stage, ShaderStage::Vertex);
  EXPECT_EQ(fragment.name.view(), "fs_main");
  EXPECT_EQ(fragment.stage, ShaderStage::Fragment);
  ASSERT_EQ(vertex.inputCount, 1u);
  ASSERT_EQ(vertex.outputCount, 2u);
  ASSERT_EQ(fragment.inputCount, 2u);
  ASSERT_EQ(fragment.outputCount, 1u);
  EXPECT_EQ(shader.interfaceVariables[vertex.firstInput].builtin, ShaderBuiltin::VertexIndex);
  EXPECT_EQ(shader.interfaceVariables[vertex.firstOutput].builtin, ShaderBuiltin::Position);
  EXPECT_EQ(shader.interfaceVariables[vertex.firstOutput + 1].location, 0u);
  EXPECT_EQ(shader.interfaceVariables[fragment.firstInput].builtin, ShaderBuiltin::Position);
  EXPECT_EQ(shader.interfaceVariables[fragment.firstOutput].location, 0u);
  EXPECT_THAT(MakeBindingLayout(shader), testing::IsEmpty());
  const auto descriptor = MakeShaderDescriptor(shader, ShaderSourceKind::Spirv, "graphics");
  EXPECT_THAT(descriptor.computeEntryPoints, testing::IsEmpty());
  ASSERT_TRUE(descriptor.bufferBindings.has_value());
  EXPECT_THAT(*descriptor.bufferBindings, testing::IsEmpty());
}

TEST(GraphicsCompiler, FrozenAndOrdinaryGraphicsEmissionAgree) {
  const auto& shader = tests::GraphicsShader();
  const auto parsed = Parse(tests::kGraphicsSource.view());
  ASSERT_TRUE(parsed.hasResult());
  std::array<char, kMaxTextEmitBytes> text{};
  TextSink sink{text.data(), uint32_t(text.size())};
  ASSERT_TRUE(EmitMsl(parsed.module, sink).ok());
  EXPECT_EQ(sink.view(), shader.msl);
  std::array<uint32_t, 24576> words{};
  SpirvSink binary{words.data(), uint32_t(words.size())};
  ASSERT_TRUE(EmitSpirv(parsed.module, binary).isSuccess());
  EXPECT_THAT(std::span(words.data(), binary.size), testing::ElementsAreArray(shader.spirv));
}

TEST(GraphicsCompiler, ReflectsMatrixShapeStrideAndResourceVisibility) {
  const auto& shader = tests::MatrixShader();
  ASSERT_THAT(shader.members, SizeIs(1));
  EXPECT_EQ(shader.members[0].matrixColumns, 4u);
  EXPECT_EQ(shader.members[0].lanes, 4u);
  EXPECT_EQ(shader.members[0].matrixStrideBytes, 16u);
  EXPECT_TRUE(shader.matchesMember("params", "mvp", 0, 64, ShaderScalarType::F32, 4, 0, 0, 4, 16));
  EXPECT_FALSE(shader.matchesMember("params", "mvp", 0, 64, ShaderScalarType::F32, 4, 0, 0, 4, 8));
  const auto layout = MakeBindingLayout(shader);
  ASSERT_THAT(layout, SizeIs(1));
  EXPECT_EQ(layout[0].visibility, ShaderStage::Vertex);
  const auto descriptor = MakeShaderDescriptor(shader, ShaderSourceKind::Msl, "matrix");
  ASSERT_TRUE(descriptor.bufferBindings.has_value());
  ASSERT_THAT(*descriptor.bufferBindings, SizeIs(1));
  EXPECT_EQ(descriptor.bufferBindings->front().entryPoint, "vs_main");
  EXPECT_EQ(descriptor.bufferBindings->front().stage, ShaderStage::Vertex);
  EXPECT_EQ(descriptor.bufferBindings->front().minSizeBytes, 64u);
}

TEST(GraphicsCompiler, NonSquareMatrixOperationsMatchOrdinaryEmission) {
  const auto& shader = tests::MatrixOperationsShader();
  const auto parsed = Parse(tests::kMatrixOperationsSource.view());
  ASSERT_TRUE(parsed.hasResult());
  std::array<char, kMaxTextEmitBytes> text{};
  TextSink sink{text.data(), uint32_t(text.size())};
  ASSERT_TRUE(EmitMsl(parsed.module, sink).ok());
  EXPECT_EQ(sink.view(), shader.msl);
  std::array<uint32_t, 24576> words{};
  SpirvSink binary{words.data(), uint32_t(words.size())};
  ASSERT_TRUE(EmitSpirv(parsed.module, binary).isSuccess());
  EXPECT_THAT(std::span(words.data(), binary.size), testing::ElementsAreArray(shader.spirv));
}

TEST(GraphicsCompiler, RejectsMatrixShapeIndexAndUnsupportedLayoutCases) {
  constexpr std::string_view cases[] = {
      "fn f(m: mat2x2f) -> vec4f { return m * vec4f(0f); }",
      "fn f(m: mat2x2f) -> mat2x2f { return m + m; }",
      "fn f(m: mat2x2f) -> vec2f { return m[2i]; }",
      "fn f(m: mat2x2f) -> vec2f { return m[-1i]; }",
      "fn f(m: mat2x2f, i: i32) -> vec2f { return m[i]; }",
      "fn f(v: vec3f) -> mat2x2f { return mat2x2f(v, v); }",
      "struct P { m: mat2x2f, } @group(0) @binding(0) var<uniform> p: P;",
  };
  for (const auto source : cases) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(Parse(source).hasResult());
  }
}

TEST(GraphicsCompiler, MatrixColumnConstantsCannotBypassConstantValidation) {
  constexpr std::string_view division = "fn bad() -> f32 { return mat2x2f()[0i].x / 0f; }";
  constexpr std::string_view product =
      "fn bad() -> vec2f { return mat2x2f(vec2f(1f), vec2f(1f)) * mat2x2f()[0i]; }";
  EXPECT_EQ(Parse(division).diagnostic.code, ErrorCode::InvalidConstantExpression);
  EXPECT_EQ(Parse(product).diagnostic.code, ErrorCode::InvalidConstantExpression);
}

TEST(GraphicsCompiler, RejectsInvalidEntryInterfaces) {
  constexpr std::string_view cases[] = {
      "@vertex fn v() -> @location(0) vec4<f32> { return vec4<f32>(0f); }",
      "@vertex fn v(@builtin(position) p: vec4<f32>) -> @builtin(position) vec4<f32> { return p; }",
      "@fragment fn f(@builtin(vertex_index) i: u32) {}",
      "@fragment fn f(@location(0) a: f32, @location(0) b: f32) {}",
      "@fragment fn f(@location(16) a: f32) {}",
      "@fragment fn f(@location(0) a: bool) {}",
      "@fragment fn f(@location(0) a: u32) {}",
      "@fragment fn f(a: f32) {}",
      "@fragment fn f() -> @builtin(position) vec4<f32> { return vec4<f32>(0f); }",
      "@compute @workgroup_size(1) fn c(@location(0) a: f32) {}",
      "@vertex @fragment fn v() {}",
      "@vertex @workgroup_size(1) fn v() {}",
      "fn h(@location(0) a: f32) {}",
      "fn h() -> @location(0) f32 { return 0f; }",
      "struct S { @builtin(position) @location(0) p: vec4<f32>, } "
      "@vertex fn v() -> S { var s: S; return s; }",
      "struct S { @location(0) a: f32, @location(0) b: f32, } @fragment fn f(s: S) {}",
      "struct S { @location(0) a: f32, b: f32, } @fragment fn f(s: S) {}",
  };
  for (const auto source : cases) {
    SCOPED_TRACE(source);
    EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::InvalidAttribute);
  }
}

TEST(GraphicsCompiler, TracksTransitiveResourceUsePerEntry) {
  constexpr auto source = R"wgsl(
struct Params { value: f32, }
@group(0) @binding(7) var<uniform> params: Params;
fn helper() -> f32 { return params.value; }
@vertex fn v() -> @builtin(position) vec4<f32> { return vec4<f32>(0f, 0f, 0f, 1f); }
@fragment fn f() -> @location(0) vec4<f32> { return vec4<f32>(helper()); }
)wgsl";
  const auto parsed = Parse(source);
  ASSERT_TRUE(parsed.hasResult());
  EXPECT_EQ(parsed.module.functions[0].resourceMask, 1u);
  EXPECT_EQ(parsed.module.functions[1].resourceMask, 0u);
  EXPECT_EQ(parsed.module.functions[2].resourceMask, 1u);
}

TEST(GraphicsCompiler, RejectsStructuredComputeInputUntilBothBackendsSupportIt) {
  constexpr auto source = R"wgsl(
struct Input { @builtin(global_invocation_id) gid: vec3<u32>, }
@compute @workgroup_size(1) fn compute_main(input: Input) {}
)wgsl";
  EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::UnsupportedConstruct);
}

TEST(GraphicsCompiler, ComputeEntryDoesNotRequireAnUnusedInvocationId) {
  const auto parsed = Parse("@compute @workgroup_size(1) fn main() {}");
  ASSERT_TRUE(parsed.hasResult());
  EXPECT_EQ(parsed.module.functions[0].inputCount, 0u);
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
