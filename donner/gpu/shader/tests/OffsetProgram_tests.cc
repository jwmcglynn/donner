/// @file
/// Reflected offset interfaces, deterministic compilation and fractional resampling contract.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <span>
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
  EXPECT_TRUE(shader.matchesMember("params", "shiftX", 0, 4, ShaderScalarType::I32));
  EXPECT_TRUE(shader.matchesMember("params", "shiftY", 4, 4, ShaderScalarType::I32));
  EXPECT_TRUE(shader.matchesMember("params", "weightX", 8, 4, ShaderScalarType::F32));
  EXPECT_TRUE(shader.matchesMember("params", "weightY", 12, 4, ShaderScalarType::F32));
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
  // The frozen WGSL projection is the authored source without comments, indentation or blank lines.
  EXPECT_EQ(frozen.wgsl.find("//"), std::string_view::npos);
  EXPECT_LT(frozen.wgsl.size(), programs::kOffsetSource.view().size());
  // The stripped projection is the same module: emitting from it reproduces the frozen bytes.
  const auto reparsed = wgsl::Parse(frozen.wgsl);
  ASSERT_TRUE(reparsed.hasResult());
  std::array<char, 8192> roundTripMsl{};
  wgsl::TextSink roundTripText{roundTripMsl.data(), uint32_t(roundTripMsl.size())};
  ASSERT_TRUE(wgsl::EmitMsl(reparsed.module, roundTripText).ok());
  EXPECT_EQ(roundTripText.view(), frozen.msl);
  std::array<uint32_t, 4096> roundTripWords{};
  wgsl::SpirvSink roundTripBinary{roundTripWords.data(), uint32_t(roundTripWords.size())};
  ASSERT_EQ(wgsl::EmitSpirv(reparsed.module, roundTripBinary).error, wgsl::SpirvEmitError::None);
  EXPECT_THAT(std::span(roundTripWords.data(), roundTripBinary.size),
              testing::ElementsAreArray(frozen.spirv));
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
TEST(OffsetProgramTests, BlendsByTheHostSplitWeightAndKeepsTransparentSourceEdges) {
  const auto source = programs::kOffsetSource.view();
  // Native image robustness would mask removal of this authored early return in pixel tests.
  EXPECT_THAT(source, testing::HasSubstr(
                          "if (((gid.x >= extent.x) || (gid.y >= extent.y))) {\n    return;\n  }"));
  EXPECT_THAT(source,
              testing::HasSubstr("let base = (coords - vec2<i32>(params.shiftX, params.shiftY));"));
  EXPECT_THAT(source, testing::Not(testing::HasSubstr("floor(")));
  EXPECT_THAT(source, testing::Not(testing::HasSubstr("round(")));
  EXPECT_THAT(source, testing::HasSubstr("return vec4<f32>(0f);"));
}
// The split is made once per shift, so every pixel blends by the same weight: a fractional shift
// of 0.6 blends input pixels x - 1 and x by 0.4, and -2.3 blends x + 2 and x + 3 by 0.3.
TEST(OffsetProgramTests, SplitsShiftsIntoAWholeBaseAndAConstantWeight) {
  using programs::SplitPixelShift;
  EXPECT_THAT(SplitPixelShift(3.0), testing::FieldsAre(3, 0.0f));
  EXPECT_THAT(SplitPixelShift(-2.0), testing::FieldsAre(-2, 0.0f));
  EXPECT_THAT(SplitPixelShift(0.6), testing::FieldsAre(1, static_cast<float>(1.0 - 0.6)));
  EXPECT_THAT(SplitPixelShift(-2.3),
              testing::FieldsAre(-2, static_cast<float>(1.0 - (-2.3 - std::floor(-2.3)))));
}
}  // namespace
}  // namespace donner::gpu::shader
