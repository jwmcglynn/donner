#include "donner/gpu/shader/wgsl/Compiler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "donner/gpu/shader/programs/GaussianBlurSource.h"

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
    if (!matches) continue;
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
  static_assert(shader.wgsl == programs::kGaussianBlurSource.view());
  static_assert(!shader.msl.empty());
  static_assert(shader.spirv[0] == 0x07230203u);
  static_assert(shader.spirv[1] == 0x00010300u);
  static_assert(shader.resources.size() == 3);
  static_assert(shader.members.size() == 10);
  static_assert(shader.entryPoints.front().name.view() == "cs_main");

  EXPECT_EQ(shader.wgsl, programs::kGaussianBlurSource.view());
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

TEST(Compiler, OrdinaryEvaluationMatchesFrozenProjections) {
  const std::string source(programs::kGaussianBlurSource.view());
  const ParseResult parsed = Parse(source);
  ASSERT_TRUE(parsed.hasResult());

  std::array<char, kGaussianArtifact.msl.size()> text{};
  TextSink textSink{text.data(), uint32_t(text.size())};
  ASSERT_TRUE(EmitMsl(parsed.module, textSink).ok());
  EXPECT_EQ(std::string_view(text.data(), textSink.size), kGaussianArtifact.view().msl);

  std::array<uint32_t, kGaussianArtifact.spirv.size()> words{};
  SpirvSink wordSink{words.data(), uint32_t(words.size())};
  ASSERT_TRUE(EmitSpirv(parsed.module, wordSink).isSuccess());
  ASSERT_EQ(wordSink.size, words.size());
  EXPECT_EQ(words, kGaussianArtifact.spirv);
}

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
