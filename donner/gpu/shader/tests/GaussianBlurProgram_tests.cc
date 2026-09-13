/// @file
/// Gaussian blur frozen-artifact metadata and projection tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/tests/CompiledGaussian.h"

namespace donner::gpu::shader {
namespace {

TEST(GaussianBlurProgramTests, FreezesAllProjectionsAndDerivedMetadata) {
  const CompiledShaderView& shader = tests::GaussianBlurAllProjections();

  EXPECT_FALSE(shader.wgsl.empty());
  EXPECT_FALSE(shader.msl.empty());
  EXPECT_FALSE(shader.spirv.empty());
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_main");
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{8, 8, 1}));
  ASSERT_NE(shader.resource("inputTexture"), nullptr);
  ASSERT_NE(shader.resource("outputTexture"), nullptr);
  ASSERT_NE(shader.resource("params"), nullptr);
  EXPECT_EQ(shader.resource("inputTexture")->binding, 0u);
  EXPECT_EQ(shader.resource("outputTexture")->binding, 1u);
  EXPECT_EQ(shader.resource("params")->binding, 2u);
  EXPECT_TRUE(shader.matchesMember("params", "stdDeviation", 0, 4, ShaderScalarType::F32));
  EXPECT_TRUE(shader.matchesMember("params", "clipMin", 24, 8, ShaderScalarType::I32, 2));
  EXPECT_TRUE(shader.matchesMember("params", "pad", 44, 4, ShaderScalarType::U32));
  EXPECT_NE(shader.wgsl.find("fn sampleEdge"), std::string_view::npos);
  EXPECT_NE(shader.msl.find("donner_msl_texture_load"), std::string_view::npos);
  EXPECT_EQ(shader.spirv.front(), 0x07230203u);
}

}  // namespace
}  // namespace donner::gpu::shader
