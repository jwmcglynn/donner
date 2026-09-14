/// @file
/// Specular lighting frozen-artifact metadata, projection agreement and source guards.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/LightingParams.h"
#include "donner/gpu/shader/programs/SpecularLightingSource.h"
#include "donner/gpu/shader/tests/CompiledSpecularLighting.h"

namespace donner::gpu::shader {
namespace {

TEST(SpecularLightingProgramTests, ReflectsStorageLayoutAndComputeSurface) {
  const auto& shader = tests::SpecularLightingAllProjections();
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_main");
  EXPECT_EQ(shader.entryPoints.front().stage, ShaderStage::Compute);
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{8, 8, 1}));
  ASSERT_THAT(shader.resources, testing::SizeIs(3));
  const auto* params = shader.resource("params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(params->minSizeBytes, sizeof(programs::LightingParams));
  EXPECT_EQ(params->binding, 2u);
  EXPECT_EQ(params->type, BindingType::ReadOnlyStorageBuffer);
  EXPECT_TRUE(shader.matchesMember("params", "specularExponent", 8, 4, ShaderScalarType::F32));
}
TEST(SpecularLightingProgramTests, OrdinaryAndFrozenNativeProjectionsAgree) {
  const auto parsed = wgsl::Parse(programs::kSpecularLightingSource.view());
  ASSERT_TRUE(parsed.hasResult());
  const auto& frozen = tests::SpecularLightingAllProjections();
  std::array<char, 32768> msl{};
  wgsl::TextSink text{msl.data(), uint32_t(msl.size())};
  ASSERT_TRUE(wgsl::EmitMsl(parsed.module, text).ok());
  EXPECT_EQ(text.view(), frozen.msl);
  std::array<uint32_t, 8192> words{};
  wgsl::SpirvSink binary{words.data(), uint32_t(words.size())};
  ASSERT_EQ(wgsl::EmitSpirv(parsed.module, binary).error, wgsl::SpirvEmitError::None);
  EXPECT_THAT(std::span(words.data(), binary.size), testing::ElementsAreArray(frozen.spirv));
  EXPECT_EQ(frozen.wgsl, programs::kSpecularLightingSource.view());
}
TEST(SpecularLightingProgramTests, MutationPropagatesThroughDescriptorAndBindingLayout) {
  const auto& shader = tests::SpecularLightingMutatedAllProjections();
  const auto descriptor = MakeShaderDescriptor(shader, ShaderSourceKind::Wgsl, "specular lighting");
  ASSERT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1));
  EXPECT_EQ(descriptor.computeEntryPoints.front().workgroupSize,
            (::donner::gpu::WorkgroupSize{4, 2, 1}));
  const auto layout = MakeBindingLayout(shader);
  ASSERT_THAT(layout, testing::SizeIs(3));
  EXPECT_EQ(layout.back().binding, 7u);
  ASSERT_TRUE(descriptor.bufferBindings.has_value());
  ASSERT_THAT(*descriptor.bufferBindings, testing::SizeIs(1));
  EXPECT_EQ(descriptor.bufferBindings->front().binding, 7u);
}
TEST(SpecularLightingProgramTests, PreservesZeroExponentAndMaximumColorAlpha) {
  const auto source = programs::kSpecularLightingSource.view();
  EXPECT_THAT(source, testing::HasSubstr("if ((exponent == 0f)) {\n    return 1f;"));
  EXPECT_THAT(source, testing::HasSubstr("max(red, max(green, blue))"));
}

}  // namespace
}  // namespace donner::gpu::shader
