/// @file
/// Frozen turbulence compilation, reflected storage and dispatch contracts.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>

#include "donner/gpu/shader/programs/TurbulenceSource.h"
#include "donner/gpu/shader/tests/CompiledTurbulence.h"
namespace donner::gpu::shader {
namespace {
TEST(TurbulenceProgramTests, StorageLayoutsMatchHostParameterAndTableBytes) {
  const auto& shader = tests::TurbulenceAllProjections();
  ASSERT_NE(shader.resource("params"), nullptr);
  ASSERT_NE(shader.resource("tables"), nullptr);
  EXPECT_EQ(shader.resource("params")->minSizeBytes, 48u);
  EXPECT_EQ(shader.resource("tables")->minSizeBytes, 18504u);
  EXPECT_TRUE(shader.matchesMember("tables", "lattice", 0, 2056, ShaderScalarType::I32, 1, 514, 4));
  EXPECT_TRUE(
      shader.matchesMember("tables", "gradX", 2056, 8224, ShaderScalarType::F32, 1, 2056, 4));
  EXPECT_TRUE(
      shader.matchesMember("tables", "gradY", 10280, 8224, ShaderScalarType::F32, 1, 2056, 4));
}
TEST(TurbulenceProgramTests, OrdinaryAndFrozenNativeProjectionsAgree) {
  const auto parsed = wgsl::Parse(programs::kTurbulenceSource.view());
  ASSERT_TRUE(parsed.hasResult());
  const auto& frozen = tests::TurbulenceAllProjections();
  std::array<char, 32768> msl{};
  wgsl::TextSink text{msl.data(), uint32_t(msl.size())};
  ASSERT_TRUE(wgsl::EmitMsl(parsed.module, text).ok());
  EXPECT_EQ(text.view(), frozen.msl);
  std::array<uint32_t, 8192> words{};
  wgsl::SpirvSink binary{words.data(), uint32_t(words.size())};
  ASSERT_EQ(wgsl::EmitSpirv(parsed.module, binary).error, wgsl::SpirvEmitError::None);
  EXPECT_THAT(std::span(words.data(), binary.size), testing::ElementsAreArray(frozen.spirv));
  // The frozen WGSL projection is the authored source without comments, indentation or blank lines.
  EXPECT_EQ(frozen.wgsl.find("//"), std::string_view::npos);
  EXPECT_LT(frozen.wgsl.size(), programs::kTurbulenceSource.view().size());
  // The stripped projection is the same module: emitting from it reproduces the frozen bytes.
  const auto reparsed = wgsl::Parse(frozen.wgsl);
  ASSERT_TRUE(reparsed.hasResult());
  std::array<char, 32768> roundTripMsl{};
  wgsl::TextSink roundTripText{roundTripMsl.data(), uint32_t(roundTripMsl.size())};
  ASSERT_TRUE(wgsl::EmitMsl(reparsed.module, roundTripText).ok());
  EXPECT_EQ(roundTripText.view(), frozen.msl);
  std::array<uint32_t, 8192> roundTripWords{};
  wgsl::SpirvSink roundTripBinary{roundTripWords.data(), uint32_t(roundTripWords.size())};
  ASSERT_EQ(wgsl::EmitSpirv(reparsed.module, roundTripBinary).error, wgsl::SpirvEmitError::None);
  EXPECT_THAT(std::span(roundTripWords.data(), roundTripBinary.size),
              testing::ElementsAreArray(frozen.spirv));
}
TEST(TurbulenceProgramTests, MutationPropagatesThroughDescriptorAndBindingLayout) {
  const auto& shader = tests::TurbulenceMutatedAllProjections();
  const auto descriptor = MakeShaderDescriptor(shader, ShaderSourceKind::Wgsl, "offset");
  ASSERT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1));
  EXPECT_EQ(descriptor.computeEntryPoints.front().workgroupSize,
            (::donner::gpu::WorkgroupSize{4, 2, 1}));
  const auto layout = MakeBindingLayout(shader);
  ASSERT_THAT(layout, testing::SizeIs(3));
  EXPECT_EQ(layout[1].binding, 7u);
  EXPECT_EQ(layout[2].binding, 6u);
  ASSERT_TRUE(descriptor.bufferBindings.has_value());
  ASSERT_THAT(*descriptor.bufferBindings, testing::SizeIs(2));
  EXPECT_EQ(descriptor.bufferBindings->front().binding, 7u);
}
}  // namespace
}  // namespace donner::gpu::shader
