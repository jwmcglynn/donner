/// @file
/// Checkerboard frozen-artifact metadata and projection tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "donner/gpu/shader/programs/Checkerboard.h"
#include "donner/gpu/shader/tests/CompiledCheckerboard.h"

namespace donner::gpu::shader {
namespace {

TEST(CheckerboardProgramTests, FreezesAllProjectionsAndDerivedMetadata) {
  const CompiledShaderView& shader = tests::CheckerboardAllProjections();

  EXPECT_THAT(shader.wgsl, testing::Not(testing::IsEmpty()));
  EXPECT_THAT(shader.msl, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(shader.spirv, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(2));
  EXPECT_EQ(shader.entryPoints[0].name.view(), "vs_main");
  EXPECT_EQ(shader.entryPoints[0].stage, ShaderStage::Vertex);
  EXPECT_EQ(shader.entryPoints[1].name.view(), "fs_main");
  EXPECT_EQ(shader.entryPoints[1].stage, ShaderStage::Fragment);
  ASSERT_THAT(shader.resources, testing::SizeIs(1));
  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("params")->group, 0u);
  EXPECT_EQ(shader.resource("params")->binding, 0u);
  EXPECT_EQ(shader.resource("params")->minSizeBytes, sizeof(programs::CheckerboardParams));
  EXPECT_NE(shader.wgsl.find("struct CheckerboardParams"), std::string_view::npos);
  EXPECT_EQ(shader.spirv.front(), 0x07230203u);
}

TEST(CheckerboardProgramTests, AdapterArtifactRetainsOnlyWgsl) {
  const CompiledShaderView& adapter = programs::CheckerboardShader();
  EXPECT_EQ(adapter.wgsl, tests::CheckerboardAllProjections().wgsl);
  EXPECT_TRUE(adapter.msl.empty());
  EXPECT_TRUE(adapter.spirv.empty());
  EXPECT_EQ(adapter.resources.size(), tests::CheckerboardAllProjections().resources.size());
  EXPECT_EQ(adapter.entryPoints.size(), tests::CheckerboardAllProjections().entryPoints.size());
}

TEST(CheckerboardProgramTests, MutationControlReflectsChangedInterface) {
  const CompiledShaderView& shader = tests::CheckerboardMutatedAllProjections();
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(2));
  EXPECT_EQ(shader.entryPoints[0].name.view(), "vs_test");
  EXPECT_EQ(shader.entryPoints[1].name.view(), "fs_test");
  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("params")->binding, 4u);
}

}  // namespace
}  // namespace donner::gpu::shader
