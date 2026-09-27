#include "donner/gpu/shader/wgsl/Compiler.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/shader/programs/GaussianBlurSource.h"
#include "donner/gpu/shader/wgsl/tests/ControlSource.h"
#include "donner/gpu/shader/wgsl/tests/GraphicsArtifact.h"
#include "donner/gpu/shader/wgsl/tests/GraphicsSource.h"
#include "donner/gpu/shader/wgsl/tests/MatrixSource.h"
#include "donner/gpu/shader/wgsl/tests/StorageArraySource.h"

namespace donner::gpu::shader::wgsl {
namespace {

template <size_t N>
consteval SourceText<N> ReplaceExact(SourceText<N> source, std::string_view oldText,
                                     std::string_view newText) {
  static_assert(N > 1);
  if (oldText.size() != newText.size()) {
    return source;
  }
  for (size_t offset = 0; offset + oldText.size() <= source.view().size(); ++offset) {
    bool matches = true;
    for (size_t index = 0; index < oldText.size(); ++index) {
      matches = matches && source.bytes[offset + index] == oldText[index];
    }
    if (!matches) {
      continue;
    }
    for (size_t index = 0; index < newText.size(); ++index) {
      source.bytes[offset + index] = newText[index];
    }
    return source;
  }
  return source;
}

constexpr auto kGaussianArtifact = Compile<programs::kGaussianBlurSource, Projection::All>();
constexpr auto kRepeatedGaussianArtifact =
    Compile<programs::kGaussianBlurSource, Projection::All>();
constexpr auto kBindingMutation =
    ReplaceExact(programs::kGaussianBlurSource, "@binding(2)", "@binding(7)");
constexpr auto kBindingArtifact = Compile<kBindingMutation, Projection::All>();
constexpr auto kBindingAndWorkgroupMutation =
    ReplaceExact(kBindingMutation, "@workgroup_size(8, 8, 1)", "@workgroup_size(4, 2, 1)");
constexpr auto kBindingAndWorkgroupArtifact =
    Compile<kBindingAndWorkgroupMutation, Projection::All>();
constexpr auto kPadTypeMutation =
    ReplaceExact(programs::kGaussianBlurSource, "pad: u32,", "pad: i32,");
constexpr auto kPadTypeArtifact = Compile<kPadTypeMutation, Projection::All>();

TEST(Compiler, FreezesAllGaussianBlurProjectionsAndMetadata) {
  constexpr CompiledShaderView shader = kGaussianArtifact.view();
  static_assert(!shader.wgsl.empty() &&
                shader.wgsl.size() < programs::kGaussianBlurSource.view().size());
  static_assert(shader.wgsl.find("//") == std::string_view::npos);
  static_assert(!shader.msl.empty());
  static_assert(shader.spirv[0] == 0x07230203u);
  static_assert(shader.spirv[1] == 0x00010300u);
  static_assert(shader.resources.size() == 3);
  static_assert(shader.members.size() == 10);
  static_assert(shader.entryPoints.front().name.view() == "cs_main");

  EXPECT_TRUE(Parse(shader.wgsl).hasResult());
  constexpr CompiledShaderView repeated = kRepeatedGaussianArtifact.view();
  EXPECT_EQ(shader.wgsl, repeated.wgsl);
  EXPECT_EQ(shader.msl, repeated.msl);
  EXPECT_TRUE(std::equal(shader.spirv.begin(), shader.spirv.end(), repeated.spirv.begin(),
                         repeated.spirv.end()));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_main");
  EXPECT_EQ(shader.entryPoints.front().workgroupSize[0], 8u);
  EXPECT_EQ(shader.entryPoints.front().workgroupSize[1], 8u);
  EXPECT_EQ(shader.entryPoints.front().workgroupSize[2], 1u);
  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("params")->binding, 2u);
  EXPECT_TRUE(shader.matchesMember("params", "pad", 44, 4, ShaderScalarType::U32));
}

TEST(Compiler, ReflectionTracksBindingAndWorkgroupMutations) {
  constexpr CompiledShaderView shader = kBindingAndWorkgroupArtifact.view();
  static_assert(shader.resource("params")->binding == 7);
  static_assert(shader.entryPoints.front().workgroupSize == std::array<uint32_t, 3>{4, 2, 1});

  const auto descriptor = MakeShaderDescriptor(shader, ShaderSourceKind::Msl, "changed dispatch");
  ASSERT_TRUE(descriptor.bufferBindings.has_value());
  ASSERT_EQ(descriptor.bufferBindings->size(), 1u);
  EXPECT_EQ(descriptor.bufferBindings->front().binding, 7u);
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{4, 2, 1}));
}

struct ProjectionCase {
  const char* name;
  std::string_view authoredSource;
  CompiledShaderView frozen;
};

void PrintTo(const ProjectionCase& value, std::ostream* os) {
  *os << value.name;
}

class CompilerProjectionRoundTrip : public testing::TestWithParam<ProjectionCase> {};

void ExpectNativeProjectionsMatch(std::string_view source, const CompiledShaderView& frozen) {
  const std::string runtimeSource(source);
  const ParseResult parsed = Parse(runtimeSource);
  ASSERT_THAT(parsed.diagnostic.code, testing::Eq(ErrorCode::None))
      << "diagnostic span [" << parsed.diagnostic.span.begin << ", " << parsed.diagnostic.span.end
      << ")";

  std::vector<char> text(frozen.msl.size());
  TextSink textSink{text.data(), static_cast<uint32_t>(text.size())};
  ASSERT_THAT(EmitMsl(parsed.module, textSink).error, testing::Eq(TextEmitError::None));
  EXPECT_THAT(textSink.view(), testing::Eq(frozen.msl));

  std::vector<uint32_t> words(frozen.spirv.size());
  SpirvSink wordSink{words.data(), static_cast<uint32_t>(words.size())};
  ASSERT_THAT(EmitSpirv(parsed.module, wordSink).error, testing::Eq(SpirvEmitError::None));
  ASSERT_THAT(wordSink.size, testing::Eq(words.size()));
  EXPECT_THAT(words, testing::ElementsAreArray(frozen.spirv));
}

TEST_P(CompilerProjectionRoundTrip, FrozenWgslReparsesToTheFrozenNativeBytes) {
  const auto& shader = GetParam().frozen;
  EXPECT_THAT(shader.wgsl, testing::Not(testing::HasSubstr("//")));
  EXPECT_THAT(shader.wgsl, testing::Not(testing::HasSubstr("\n ")));
  EXPECT_THAT(shader.wgsl, testing::Not(testing::HasSubstr("\n\n")));
  ExpectNativeProjectionsMatch(shader.wgsl, shader);
}

TEST_P(CompilerProjectionRoundTrip, OrdinaryEvaluationMatchesFrozenProjections) {
  const auto& shader = GetParam();
  ExpectNativeProjectionsMatch(shader.authoredSource, shader.frozen);
}

INSTANTIATE_TEST_SUITE_P(
    ShaderFixtures, CompilerProjectionRoundTrip,
    testing::Values(
        ProjectionCase{"GaussianBlur", programs::kGaussianBlurSource.view(),
                       kGaussianArtifact.view()},
        ProjectionCase{"Graphics", tests::kGraphicsSource.view(), tests::GraphicsShader()},
        ProjectionCase{"Matrix", tests::kMatrixSource.view(), tests::MatrixShader()},
        ProjectionCase{"MatrixOperations", tests::kMatrixOperationsSource.view(),
                       tests::MatrixOperationsShader()},
        ProjectionCase{"StorageArrays", tests::kStorageArraySource.view(),
                       tests::StorageArrayShader()},
        ProjectionCase{"ControlFlow", tests::kControlSource.view(), tests::ControlShader()},
        ProjectionCase{"SamplingSwitch", tests::kSamplingSwitchSource.view(),
                       tests::SamplingSwitchShader()}),
    [](const testing::TestParamInfo<ProjectionCase>& info) { return info.param.name; });

TEST(Compiler, BindingMutationUpdatesDerivedReflection) {
  constexpr CompiledShaderView shader = kBindingArtifact.view();
  static_assert(shader.resource("params") != nullptr);
  static_assert(shader.resource("params")->binding == 7);

  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("params")->binding, 7u);
  EXPECT_NE(shader.wgsl.find("@binding(7) var<uniform> params"), std::string_view::npos);
  EXPECT_NE(shader.msl.find("[[buffer(8)]]"), std::string_view::npos);
  const auto layout = MakeBindingLayout(shader);
  ASSERT_EQ(layout.size(), 3u);
  EXPECT_EQ(layout[2].binding, 7u);
  EXPECT_EQ(layout[2].type, BindingType::UniformBuffer);
  const auto descriptor = MakeShaderDescriptor(shader, ShaderSourceKind::Msl, "changed binding");
  ASSERT_TRUE(descriptor.bufferBindings.has_value());
  ASSERT_EQ(descriptor.bufferBindings->size(), 1u);
  EXPECT_EQ(descriptor.bufferBindings->front().binding, 7u);
  EXPECT_EQ(descriptor.bufferBindings->front().minSizeBytes, 48u);
}

TEST(Compiler, UniformTypeMutationCannotMatchTheOldHostField) {
  constexpr CompiledShaderView shader = kPadTypeArtifact.view();
  static_assert(shader.matchesMember("params", "pad", 44, 4, ShaderScalarType::I32));
  static_assert(!shader.matchesMember("params", "pad", 44, 4, ShaderScalarType::U32));

  EXPECT_TRUE(shader.matchesMember("params", "pad", 44, 4, ShaderScalarType::I32));
  EXPECT_FALSE(shader.matchesMember("params", "pad", 44, 4, ShaderScalarType::U32));
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
