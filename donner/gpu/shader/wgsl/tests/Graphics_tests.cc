#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

#include "donner/gpu/shader/wgsl/Compiler.h"
#include "donner/gpu/shader/wgsl/tests/GraphicsArtifact.h"
#include "donner/gpu/shader/wgsl/tests/GraphicsSource.h"
#include "donner/gpu/shader/wgsl/tests/MatrixSource.h"
#include "donner/gpu/shader/wgsl/tests/StorageArraySource.h"

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

TEST(GraphicsCompiler, ReflectsRuntimeStridesIntoDeviceBufferRequirements) {
  const auto& shader = tests::StorageArrayShader();
  ASSERT_THAT(shader.resources, SizeIs(4));
  ASSERT_NE(shader.resource("bands"), nullptr);
  EXPECT_EQ(shader.resource("bands")->minSizeBytes, 8u);
  EXPECT_EQ(shader.resource("bands")->runtimeArrayStrideBytes, 8u);
  EXPECT_EQ(shader.resource("bands")->runtimeArrayLanes, 0u);
  ASSERT_NE(shader.resource("values"), nullptr);
  EXPECT_EQ(shader.resource("values")->runtimeArrayScalarType, ShaderScalarType::F32);
  EXPECT_EQ(shader.resource("values")->runtimeArrayLanes, 1u);
  ASSERT_NE(shader.resource("indices"), nullptr);
  EXPECT_EQ(shader.resource("indices")->runtimeArrayScalarType, ShaderScalarType::U32);
  EXPECT_EQ(shader.resource("indices")->runtimeArrayLanes, 1u);
  EXPECT_TRUE(shader.matchesMember("bands", "count", 4, 4, ShaderScalarType::U32));
  EXPECT_TRUE(shader.matchesMember("params", "vertices", 0, 64, ShaderScalarType::F32, 4, 4, 16));
  const auto descriptor = MakeShaderDescriptor(shader, ShaderSourceKind::Msl, "storage arrays");
  ASSERT_TRUE(descriptor.bufferBindings.has_value());
  ASSERT_THAT(*descriptor.bufferBindings, SizeIs(4));
  EXPECT_EQ((*descriptor.bufferBindings)[0].runtimeArrayStrideBytes, 0u);
  EXPECT_EQ((*descriptor.bufferBindings)[1].runtimeArrayStrideBytes, 8u);
  EXPECT_EQ((*descriptor.bufferBindings)[2].runtimeArrayStrideBytes, 4u);
  EXPECT_EQ((*descriptor.bufferBindings)[3].runtimeArrayStrideBytes, 4u);
  for (const auto& binding : *descriptor.bufferBindings)
    EXPECT_EQ(binding.stage, ShaderStage::Fragment);
}

TEST(GraphicsCompiler, StorageArrayOrdinaryEmissionMatchesFrozenArtifact) {
  const auto& shader = tests::StorageArrayShader();
  const auto parsed = Parse(tests::kStorageArraySource.view());
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

TEST(GraphicsCompiler, RejectsUnsupportedArrayUsesAndConstantNegativeIndices) {
  constexpr std::string_view cases[] = {
      "@group(0) @binding(0) var<uniform> data: array<f32>;",
      "@group(0) @binding(0) var<storage, read> data: array<array<f32>>;",
      "@group(0) @binding(0) var<storage, read_write> data: array<f32>;",
      "@group(0) @binding(0) var<storage, read> data: array<bool>;",
      "struct P { data: array<f32, 4>, } @group(0) @binding(0) var<uniform> p: P;",
      "struct P { data: array<f32>, }",
      "fn f(data: array<f32>) {}",
      "@group(0) @binding(0) var<storage, read> data: array<f32>; fn f() { let copy = data; }",
      "@group(0) @binding(0) var<storage, read> data: array<f32>; fn f() { data[0i] = 1f; }",
      "@group(0) @binding(0) var<storage, read> data: array<f32>; fn f() -> f32 { return "
      "data[-1i]; }",
  };
  for (auto source : cases) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(Parse(source).hasResult());
  }
  EXPECT_TRUE(Parse("@group(0) @binding(0) var<storage, read> data: array<f32>; "
                    "fn first() -> f32 { return data[0u]; }")
                  .hasResult());
}

/// One vertex entry reading a float, an unsigned and a signed attribute, so a single artifact
/// exercises every numeric vertex-input type through all three projections.
inline constexpr SourceText kIntegerVertexAttributeSource{
    R"wgsl(@vertex
fn vs_attributes(@location(0) position: f32, @location(1) unsignedValue: u32, @location(2) signedValue: i32) -> @builtin(position) vec4<f32> {
  return vec4<f32>(position, f32(unsignedValue), f32(signedValue), 1f);
}
)wgsl"};
constexpr auto kIntegerVertexAttributeArtifact =
    Compile<kIntegerVertexAttributeSource, Projection::All>();

TEST(GraphicsCompiler, EmitsNumericVertexAttributesInEveryProjection) {
  constexpr CompiledShaderView shader = kIntegerVertexAttributeArtifact.view();
  static_assert(!shader.wgsl.empty());
  static_assert(!shader.msl.empty());
  static_assert(!shader.spirv.empty());
  static_assert(shader.entryPoints.size() == 1);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Vertex);
  static_assert(shader.entryPoints[0].inputCount == 3);

  const auto& inputs = shader.interfaceVariables;
  const uint32_t first = shader.entryPoints[0].firstInput;
  EXPECT_EQ(inputs[first].scalarType, ShaderScalarType::F32);
  EXPECT_EQ(inputs[first].location, 0u);
  EXPECT_FALSE(inputs[first].flat);
  EXPECT_EQ(inputs[first + 1].scalarType, ShaderScalarType::U32);
  EXPECT_EQ(inputs[first + 1].location, 1u);
  EXPECT_FALSE(inputs[first + 1].flat);
  EXPECT_EQ(inputs[first + 2].scalarType, ShaderScalarType::I32);
  EXPECT_EQ(inputs[first + 2].location, 2u);
  EXPECT_FALSE(inputs[first + 2].flat);
}

/// Source drawing one vertex attribute of `type`, the narrowest module that exercises a vertex
/// input's accepted numeric types.
/// @param type WGSL scalar type of the attribute.
std::string VertexAttributeSource(std::string_view type) {
  return std::string("@vertex fn v(@location(0) value: ") + std::string(type) +
         ") -> @builtin(position) vec4<f32> { return vec4<f32>(f32(value), 0f, 0f, 1f); }";
}

TEST(GraphicsCompiler, AcceptsNumericVertexAttributesWithoutAnInterpolationQualifier) {
  // A vertex attribute is read from a buffer rather than interpolated across a primitive, so the
  // integer types are accepted here even though an integer interstage value needs flat.
  for (const std::string_view type : {"f32", "u32", "i32"}) {
    SCOPED_TRACE(type);
    const ParseResult parsed = Parse(VertexAttributeSource(type));
    EXPECT_EQ(parsed.diagnostic.code, ErrorCode::None)
        << "actual code " << unsigned(parsed.diagnostic.code);
    EXPECT_TRUE(parsed.hasResult());
  }
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
      // A vertex attribute is fetched, not interpolated, so an interpolation qualifier on one is
      // not merely redundant: it names a rate that does not exist for this input.
      "@vertex fn v(@location(0) @interpolate(flat) a: u32) -> @builtin(position) vec4<f32> "
      "{ return vec4<f32>(0f); }",
      "@vertex fn v(@location(0) @interpolate(flat) a: f32) -> @builtin(position) vec4<f32> "
      "{ return vec4<f32>(0f); }",
      // A fragment output goes to a color attachment, and every render target format this
      // runtime accepts is float.
      "@fragment fn f() -> @location(0) u32 { return 0u; }",
      "@fragment fn f() -> @location(0) i32 { return 0i; }",
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
