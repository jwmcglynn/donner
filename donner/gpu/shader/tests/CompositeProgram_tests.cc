/// @file
/// Composite frozen-artifact metadata and projection tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "donner/gpu/shader/programs/Composite.h"
#include "donner/gpu/shader/tests/CompiledComposite.h"

namespace donner::gpu::shader {
namespace {

TEST(CompositeProgramTests, FreezesAllProjectionsAndDerivedMetadata) {
  const CompiledShaderView& shader = tests::CompositeAllProjections();

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
  ASSERT_NE(shader.resource("destinationTexture"), nullptr);
  EXPECT_EQ(shader.resource("destinationTexture")->group, 0u);
  EXPECT_EQ(shader.resource("destinationTexture")->binding, 1u);
  ASSERT_NE(shader.resource("outputTexture"), nullptr);
  EXPECT_EQ(shader.resource("outputTexture")->group, 0u);
  EXPECT_EQ(shader.resource("outputTexture")->binding, 2u);
  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("params")->group, 0u);
  EXPECT_EQ(shader.resource("params")->binding, 3u);
  EXPECT_EQ(shader.resource("params")->minSizeBytes, sizeof(programs::CompositeParams));
  EXPECT_NE(shader.wgsl.find("struct CompositeParams"), std::string_view::npos);
  EXPECT_EQ(shader.spirv.front(), 0x07230203u);
}

TEST(CompositeProgramTests, AdapterArtifactRetainsOnlyWgsl) {
  const CompiledShaderView& adapter = programs::CompositeShader();
  EXPECT_EQ(adapter.wgsl, tests::CompositeAllProjections().wgsl);
  EXPECT_TRUE(adapter.msl.empty());
  EXPECT_TRUE(adapter.spirv.empty());
  EXPECT_EQ(adapter.resources.size(), tests::CompositeAllProjections().resources.size());
  EXPECT_EQ(adapter.entryPoints.size(), tests::CompositeAllProjections().entryPoints.size());
}

TEST(CompositeProgramTests, MutationControlReflectsChangedInterface) {
  const CompiledShaderView& shader = tests::CompositeMutatedAllProjections();
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_test");
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{4, 2, 1}));
  ASSERT_NE(shader.resource("sourceTexture"), nullptr);
  EXPECT_EQ(shader.resource("sourceTexture")->binding, 6u);
  ASSERT_NE(shader.resource("destinationTexture"), nullptr);
  EXPECT_EQ(shader.resource("destinationTexture")->binding, 5u);
  ASSERT_NE(shader.resource("outputTexture"), nullptr);
  EXPECT_EQ(shader.resource("outputTexture")->binding, 4u);
  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("params")->binding, 7u);
}

}  // namespace
}  // namespace donner::gpu::shader
