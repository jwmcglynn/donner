/// @file
/// FilterColorMatrix frozen-artifact metadata and projection tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/shader/tests/CompiledFilterColorMatrix.h"

namespace donner::gpu::shader {
namespace {

TEST(FilterColorMatrixProgramTests, FreezesAllProjectionsAndDerivedMetadata) {
  const CompiledShaderView& shader = tests::FilterColorMatrixAllProjections();

  EXPECT_THAT(shader.wgsl, testing::Not(testing::IsEmpty()));
  EXPECT_THAT(shader.msl, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(shader.spirv, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_main");
  EXPECT_EQ(shader.entryPoints.front().stage, ShaderStage::Compute);
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{8, 8, 1}));
  ASSERT_THAT(shader.resources, testing::SizeIs(3));
  ASSERT_NE(shader.resource("inputTexture"), nullptr);
  EXPECT_EQ(shader.resource("inputTexture")->group, 0u);
  EXPECT_EQ(shader.resource("inputTexture")->binding, 0u);
  ASSERT_NE(shader.resource("outputTexture"), nullptr);
  EXPECT_EQ(shader.resource("outputTexture")->group, 0u);
  EXPECT_EQ(shader.resource("outputTexture")->binding, 1u);
  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("params")->group, 0u);
  EXPECT_EQ(shader.resource("params")->binding, 2u);
  EXPECT_EQ(shader.resource("params")->minSizeBytes, sizeof(programs::FilterColorMatrixParams));
  EXPECT_NE(shader.wgsl.find("struct FilterColorMatrixParams"), std::string_view::npos);
  EXPECT_EQ(shader.spirv.front(), 0x07230203u);
}

TEST(FilterColorMatrixProgramTests, AdapterArtifactRetainsOnlyWgsl) {
  const CompiledShaderView& adapter = programs::FilterColorMatrixShader();
  EXPECT_EQ(adapter.wgsl, tests::FilterColorMatrixAllProjections().wgsl);
  EXPECT_TRUE(adapter.msl.empty());
  EXPECT_TRUE(adapter.spirv.empty());
  EXPECT_EQ(adapter.resources.size(), tests::FilterColorMatrixAllProjections().resources.size());
  EXPECT_EQ(adapter.entryPoints.size(),
            tests::FilterColorMatrixAllProjections().entryPoints.size());
}

TEST(FilterColorMatrixProgramTests, MutationControlReflectsChangedInterface) {
  const CompiledShaderView& shader = tests::FilterColorMatrixMutatedAllProjections();
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_test");
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{4, 2, 1}));
  ASSERT_NE(shader.resource("inputTexture"), nullptr);
  EXPECT_EQ(shader.resource("inputTexture")->binding, 6u);
  ASSERT_NE(shader.resource("outputTexture"), nullptr);
  EXPECT_EQ(shader.resource("outputTexture")->binding, 5u);
  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("params")->binding, 4u);
}

}  // namespace
}  // namespace donner::gpu::shader
