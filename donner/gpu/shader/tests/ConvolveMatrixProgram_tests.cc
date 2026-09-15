/// @file
/// Matrix-convolution interface, layout, and deterministic backend emission contracts.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/programs/ConvolveMatrix.h"
#include "donner/gpu/shader/programs/ConvolveMatrixSource.h"
#include "donner/gpu/shader/tests/CompiledConvolve.h"
#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/SpirvEmitter.h"
#include "donner/gpu/shader/wgsl/TextEmitter.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::shader {
namespace {

std::string EmitConvolveMatrixWgsl() {
  return std::string(tests::ConvolveMatrixAllProjections().wgsl);
}

TEST(ConvolveMatrixProgramTests, ModuleBuildsCleanly) {
  const CompiledShaderView& shader = tests::ConvolveMatrixAllProjections();
  EXPECT_FALSE(shader.wgsl.empty());
  EXPECT_FALSE(shader.msl.empty());
  EXPECT_FALSE(shader.spirv.empty());
}

TEST(ConvolveMatrixProgramTests, EmitsDeterministically) {
  const wgsl::ParseResult parsed = wgsl::Parse(programs::kConvolveMatrixSource.view());
  ASSERT_TRUE(parsed.hasResult());
  std::array<char, 32768> msl = {};
  wgsl::TextSink mslSink{msl.data(), static_cast<uint32_t>(msl.size())};
  ASSERT_TRUE(wgsl::EmitMsl(parsed.module, mslSink).ok());
  std::array<uint32_t, 24576> spirv = {};
  wgsl::SpirvSink spirvSink{spirv.data(), static_cast<uint32_t>(spirv.size())};
  ASSERT_TRUE(wgsl::EmitSpirv(parsed.module, spirvSink).isSuccess());
  const CompiledShaderView& frozen = tests::ConvolveMatrixAllProjections();
  EXPECT_EQ(std::string_view(msl.data(), mslSink.size), frozen.msl);
  EXPECT_TRUE(std::equal(spirv.begin(), spirv.begin() + spirvSink.size, frozen.spirv.begin(),
                         frozen.spirv.end()));
  const std::string projection = EmitConvolveMatrixWgsl();
  EXPECT_EQ(projection.find("//"), std::string::npos);
  EXPECT_LT(projection.size(), programs::kConvolveMatrixSource.view().size());
  EXPECT_TRUE(wgsl::Parse(projection).hasResult());
}

TEST(ConvolveMatrixProgramTests, NativeEmittersAcceptPortableParameterMemberNames) {
  const CompiledShaderView& shader = tests::ConvolveMatrixAllProjections();
  EXPECT_FALSE(shader.msl.empty());
  EXPECT_FALSE(shader.spirv.empty());
}

TEST(ConvolveMatrixProgramTests, GeneratedDescriptorsPreserveTheTypedInterface) {
  const CompiledShaderView& shader = tests::ConvolveMatrixAllProjections();
  for (ShaderSourceKind kind :
       {ShaderSourceKind::Wgsl, ShaderSourceKind::Msl, ShaderSourceKind::Spirv}) {
    const ShaderModuleDescriptor descriptor = MakeShaderDescriptor(shader, kind, "ConvolveMatrix");
    EXPECT_EQ(descriptor.sourceKind, kind);
    ASSERT_TRUE(descriptor.bufferBindings.has_value());
    ASSERT_EQ(descriptor.bufferBindings->size(), 1u);
    const auto& binding = descriptor.bufferBindings->front();
    EXPECT_EQ(binding.group, 0u);
    EXPECT_EQ(binding.binding, 2u);
    EXPECT_EQ(binding.stage, ShaderStage::Compute);
    EXPECT_EQ(binding.type, BindingType::ReadOnlyStorageBuffer);
    EXPECT_EQ(binding.minSizeBytes, 132u);
    EXPECT_EQ(binding.runtimeArrayStrideBytes, 0u);
    EXPECT_EQ(descriptor.sourceText.empty(), kind == ShaderSourceKind::Spirv);
    EXPECT_EQ(descriptor.spirvWords.empty(), kind != ShaderSourceKind::Spirv);
    ASSERT_EQ(descriptor.computeEntryPoints.size(), 1u);
    EXPECT_EQ(descriptor.computeEntryPoints.front().name, shader.entryPoints.front().name.view());
    EXPECT_EQ(descriptor.computeEntryPoints.front().workgroupSize.x,
              shader.entryPoints.front().workgroupSize[0]);
    EXPECT_EQ(descriptor.computeEntryPoints.front().workgroupSize.y,
              shader.entryPoints.front().workgroupSize[1]);
    EXPECT_EQ(descriptor.computeEntryPoints.front().workgroupSize.z,
              shader.entryPoints.front().workgroupSize[2]);
  }
}

TEST(ConvolveMatrixProgramTests, BridgeRejectsUnretainedNativeProjections) {
  const auto& shader = programs::ConvolveMatrixShader();
  EXPECT_TRUE(shader.msl.empty());
  EXPECT_TRUE(shader.spirv.empty());
  for (ShaderSourceKind kind : {ShaderSourceKind::Msl, ShaderSourceKind::Spirv}) {
    const auto descriptor = MakeShaderDescriptor(shader, kind, "unretained");
    EXPECT_TRUE(descriptor.sourceText.empty());
    EXPECT_TRUE(descriptor.spirvWords.empty());
    EXPECT_FALSE(descriptor.bufferBindings.has_value());
    RecordingDevice device;
    EXPECT_THAT(device.createShaderModule(descriptor), IsGpuError(GpuErrorType::InvalidDescriptor));
  }
}

TEST(ConvolveMatrixProgramTests, StorageLayoutMatchesTheUploadedParameterBlock) {
  const CompiledShaderView& shader = tests::ConvolveMatrixAllProjections();
  const ShaderResource* params = shader.resource("params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(params->type, BindingType::ReadOnlyStorageBuffer);
  EXPECT_EQ(params->minSizeBytes, sizeof(programs::ConvolveMatrixParams));
  EXPECT_EQ(params->memberCount, 9u);
  EXPECT_TRUE(
      shader.matchesMember("params", "coefficients", 32, 100, ShaderScalarType::F32, 1, 25, 4));
  const std::array<uint32_t, 9> offsets{0, 4, 8, 12, 16, 20, 24, 28, 32};
  for (size_t index = 0; index < offsets.size(); ++index) {
    EXPECT_EQ(shader.members[params->firstMember + index].offsetBytes, offsets[index]);
  }
}

}  // namespace
}  // namespace donner::gpu::shader
