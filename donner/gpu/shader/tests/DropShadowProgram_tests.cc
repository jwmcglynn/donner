/// @file
/// DropShadow frozen-artifact metadata and projection tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "donner/gpu/shader/programs/DropShadow.h"
#include "donner/gpu/shader/tests/CompiledDropShadow.h"

namespace donner::gpu::shader {
namespace {

TEST(DropShadowProgramTests, FreezesAllProjectionsAndDerivedMetadata) {
  const CompiledShaderView& shader = tests::DropShadowAllProjections();

  EXPECT_THAT(shader.wgsl, testing::Not(testing::IsEmpty()));
  EXPECT_THAT(shader.msl, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(shader.spirv, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_main");
  EXPECT_EQ(shader.entryPoints.front().stage, ShaderStage::Compute);
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{8, 8, 1}));
  ASSERT_THAT(shader.resources, testing::SizeIs(4));
  ASSERT_NE(shader.resource("sourceTexture"), nullptr);
  EXPECT_EQ(shader.resource("sourceTexture")->group, 0u);
  EXPECT_EQ(shader.resource("sourceTexture")->binding, 0u);
  ASSERT_NE(shader.resource("blurredTexture"), nullptr);
  EXPECT_EQ(shader.resource("blurredTexture")->group, 0u);
  EXPECT_EQ(shader.resource("blurredTexture")->binding, 1u);
  ASSERT_NE(shader.resource("outputTexture"), nullptr);
  EXPECT_EQ(shader.resource("outputTexture")->group, 0u);
  EXPECT_EQ(shader.resource("outputTexture")->binding, 2u);
  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("params")->group, 0u);
  EXPECT_EQ(shader.resource("params")->binding, 3u);
  EXPECT_EQ(shader.resource("params")->minSizeBytes, sizeof(programs::DropShadowParams));
  EXPECT_NE(shader.wgsl.find("struct DropShadowParams"), std::string_view::npos);
  EXPECT_EQ(shader.spirv.front(), 0x07230203u);
}

TEST(DropShadowProgramTests, AdapterArtifactRetainsOnlyWgsl) {
  const CompiledShaderView& adapter = programs::DropShadowShader();
  EXPECT_EQ(adapter.wgsl, tests::DropShadowAllProjections().wgsl);
  EXPECT_TRUE(adapter.msl.empty());
  EXPECT_TRUE(adapter.spirv.empty());
  EXPECT_EQ(adapter.resources.size(), tests::DropShadowAllProjections().resources.size());
  EXPECT_EQ(adapter.entryPoints.size(), tests::DropShadowAllProjections().entryPoints.size());
}

TEST(DropShadowProgramTests, MutationControlReflectsChangedInterface) {
  const CompiledShaderView& shader = tests::DropShadowMutatedAllProjections();
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_test");
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{4, 2, 1}));
  ASSERT_NE(shader.resource("sourceTexture"), nullptr);
  EXPECT_EQ(shader.resource("sourceTexture")->binding, 6u);
  ASSERT_NE(shader.resource("blurredTexture"), nullptr);
  EXPECT_EQ(shader.resource("blurredTexture")->binding, 5u);
  ASSERT_NE(shader.resource("outputTexture"), nullptr);
  EXPECT_EQ(shader.resource("outputTexture")->binding, 4u);
  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("params")->binding, 7u);
}

}  // namespace
}  // namespace donner::gpu::shader
