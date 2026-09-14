/// @file
/// SnapshotUnpremultiply frozen-artifact metadata and projection tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
#include "donner/gpu/shader/tests/CompiledSnapshotUnpremultiply.h"

namespace donner::gpu::shader {
namespace {

TEST(SnapshotUnpremultiplyProgramTests, FreezesAllProjectionsAndDerivedMetadata) {
  const CompiledShaderView& shader = tests::SnapshotUnpremultiplyAllProjections();

  EXPECT_THAT(shader.wgsl, testing::Not(testing::IsEmpty()));
  EXPECT_THAT(shader.msl, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(shader.spirv, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_main");
  EXPECT_EQ(shader.entryPoints.front().stage, ShaderStage::Compute);
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{8, 8, 1}));
  ASSERT_THAT(shader.resources, testing::SizeIs(2));
  ASSERT_NE(shader.resource("inputTexture"), nullptr);
  EXPECT_EQ(shader.resource("inputTexture")->group, 0u);
  EXPECT_EQ(shader.resource("inputTexture")->binding, 0u);
  ASSERT_NE(shader.resource("outputTexture"), nullptr);
  EXPECT_EQ(shader.resource("outputTexture")->group, 0u);
  EXPECT_EQ(shader.resource("outputTexture")->binding, 1u);
  EXPECT_NE(shader.wgsl.find("let halfAlpha"), std::string_view::npos);
  EXPECT_EQ(shader.spirv.front(), 0x07230203u);
}

TEST(SnapshotUnpremultiplyProgramTests, AdapterArtifactRetainsOnlyWgsl) {
  const CompiledShaderView& adapter = programs::SnapshotUnpremultiplyShader();
  EXPECT_EQ(adapter.wgsl, tests::SnapshotUnpremultiplyAllProjections().wgsl);
  EXPECT_TRUE(adapter.msl.empty());
  EXPECT_TRUE(adapter.spirv.empty());
  EXPECT_EQ(adapter.resources.size(),
            tests::SnapshotUnpremultiplyAllProjections().resources.size());
  EXPECT_EQ(adapter.entryPoints.size(),
            tests::SnapshotUnpremultiplyAllProjections().entryPoints.size());
}

TEST(SnapshotUnpremultiplyProgramTests, MutationControlReflectsChangedInterface) {
  const CompiledShaderView& shader = tests::SnapshotUnpremultiplyMutatedAllProjections();
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_test");
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{4, 2, 1}));
  ASSERT_NE(shader.resource("inputTexture"), nullptr);
  EXPECT_EQ(shader.resource("inputTexture")->binding, 6u);
  ASSERT_NE(shader.resource("outputTexture"), nullptr);
  EXPECT_EQ(shader.resource("outputTexture")->binding, 5u);
}

}  // namespace
}  // namespace donner::gpu::shader
