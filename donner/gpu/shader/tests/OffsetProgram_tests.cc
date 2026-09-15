/// @file
/// Reflected offset interfaces, deterministic compilation and half-pixel rounding contract.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>

#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/shader/programs/OffsetSource.h"
#include "donner/gpu/shader/tests/CompiledOffset.h"
namespace donner::gpu::shader {
namespace {
TEST(OffsetProgramTests, ReflectsUniformLayoutAndComputeSurface) {
  const auto& shader = tests::OffsetAllProjections();
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_main");
  EXPECT_EQ(shader.entryPoints.front().stage, ShaderStage::Compute);
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{8, 8, 1}));
  ASSERT_THAT(shader.resources, testing::SizeIs(3));
  const auto* params = shader.resource("params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(params->minSizeBytes, sizeof(programs::OffsetParams));
  EXPECT_EQ(params->binding, 2u);
  EXPECT_EQ(params->type, BindingType::UniformBuffer);
  EXPECT_TRUE(shader.matchesMember("params", "dx", 0, 4, ShaderScalarType::F32));
  EXPECT_TRUE(shader.matchesMember("params", "dy", 4, 4, ShaderScalarType::F32));
  EXPECT_TRUE(shader.matchesMember("params", "pad0", 8, 4, ShaderScalarType::U32));
  EXPECT_TRUE(shader.matchesMember("params", "pad1", 12, 4, ShaderScalarType::U32));
}
TEST(OffsetProgramTests, OrdinaryAndFrozenNativeProjectionsAgree) {
  const auto parsed = wgsl::Parse(programs::kOffsetSource.view());
  ASSERT_TRUE(parsed.hasResult());
  const auto& frozen = tests::OffsetAllProjections();
  std::array<char, 8192> msl{};
  wgsl::TextSink text{msl.data(), uint32_t(msl.size())};
  ASSERT_TRUE(wgsl::EmitMsl(parsed.module, text).ok());
  EXPECT_EQ(text.view(), frozen.msl);
  std::array<uint32_t, 4096> words{};
  wgsl::SpirvSink binary{words.data(), uint32_t(words.size())};
  ASSERT_EQ(wgsl::EmitSpirv(parsed.module, binary).error, wgsl::SpirvEmitError::None);
  EXPECT_THAT(std::span(words.data(), binary.size), testing::ElementsAreArray(frozen.spirv));
  // The frozen WGSL projection is the authored source without comments, indentation or blank
  // lines; it must still parse and stay smaller than the source.
  EXPECT_EQ(frozen.wgsl.find("//"), std::string_view::npos);
  EXPECT_LT(frozen.wgsl.size(), programs::kOffsetSource.view().size());
  EXPECT_TRUE(wgsl::Parse(frozen.wgsl).hasResult());
}
TEST(OffsetProgramTests, MutationPropagatesThroughDescriptorAndBindingLayout) {
  const auto& shader = tests::OffsetMutatedAllProjections();
  const auto descriptor = MakeShaderDescriptor(shader, ShaderSourceKind::Wgsl, "offset");
  ASSERT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1));
  EXPECT_EQ(descriptor.computeEntryPoints.front().workgroupSize, (WorkgroupSize{4, 2, 1}));
  const auto layout = MakeBindingLayout(shader);
  ASSERT_THAT(layout, testing::SizeIs(3));
  EXPECT_EQ(layout.back().binding, 7u);
  ASSERT_TRUE(descriptor.bufferBindings.has_value());
  ASSERT_THAT(*descriptor.bufferBindings, testing::SizeIs(1));
  EXPECT_EQ(descriptor.bufferBindings->front().binding, 7u);
}
TEST(OffsetProgramTests, KeepsHalfAwayRoundingAndTransparentSourceEdges) {
  const auto source = programs::kOffsetSource.view();
  EXPECT_THAT(source, testing::HasSubstr("floor(magnitude)"));
  // Native image robustness would mask removal of this authored early return in pixel tests.
  EXPECT_THAT(source, testing::HasSubstr(
                          "if (((gid.x >= extent.x) || (gid.y >= extent.y))) {\n    return;\n  }"));
  EXPECT_THAT(source, testing::HasSubstr("sign(x)"));
  EXPECT_THAT(source, testing::HasSubstr("(magnitude - integral) >= 0.5f"));
  EXPECT_THAT(source, testing::Not(testing::HasSubstr("round(")));
  EXPECT_THAT(source,
              testing::HasSubstr("textureStore(outputTexture, coords, vec4<f32>(0f, 0f, 0f, 0f))"));
}
}  // namespace
}  // namespace donner::gpu::shader
