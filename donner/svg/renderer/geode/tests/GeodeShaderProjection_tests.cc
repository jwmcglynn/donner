/// @file
/// Projection selection for the production Slug and image-blit shader module creators.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "donner/gpu/Device.h"
#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/shader/programs/SlugGradient.h"
#include "donner/gpu/shader/programs/SlugMask.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/geode/GeodeShaders.h"

namespace donner::geode {
namespace {

using testing::ElementsAreArray;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;

/// The projection a native device on this platform consumes. The native artifacts this build
/// links carry exactly this one.
constexpr gpu::ShaderSourceKind kPlatformNativeKind =
#if defined(__APPLE__)
    gpu::ShaderSourceKind::Msl;
#else
    gpu::ShaderSourceKind::Spirv;
#endif

/// The native projection no artifact this build links carries, so a device reporting it must be
/// refused rather than handed an empty source.
constexpr gpu::ShaderSourceKind kUnlinkedNativeKind =
#if defined(__APPLE__)
    gpu::ShaderSourceKind::Spirv;
#else
    gpu::ShaderSourceKind::Msl;
#endif

/**
 * Device reporting a caller-chosen shader source kind, keeping the descriptor of the shader
 * module it last accepted so a test can assert which projection a creator selected.
 *
 * Inherits every fail-closed check from \ref gpu::Device; the remaining backend operations
 * succeed without recording anything.
 */
class ProjectionCapturingDevice final : public gpu::Device {
public:
  /// @param kind Source kind this device reports to callers.
  explicit ProjectionCapturingDevice(gpu::ShaderSourceKind kind) : kind_(kind) {}

  gpu::ShaderSourceKind shaderSourceKind() const override { return kind_; }
  uint64_t completedSerial() const override { return lastSubmittedSerial(); }

  /// Descriptor of the most recently accepted shader module; default-constructed until one is.
  const gpu::ShaderModuleDescriptor& lastDescriptor() const { return lastDescriptor_; }

protected:
  gpu::Status onCreateShaderModule(uint32_t,
                                   const gpu::ShaderModuleDescriptor& descriptor) override {
    lastDescriptor_ = descriptor;
    return gpu::OkStatus();
  }

  gpu::Status onCreateBuffer(uint32_t, const gpu::BufferDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateTexture(uint32_t, const gpu::TextureDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateTextureView(uint32_t, uint32_t, const gpu::TextureViewDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateSampler(uint32_t, const gpu::SamplerDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateBindGroupLayout(uint32_t, const gpu::BindGroupLayoutDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateBindGroup(uint32_t, const gpu::BindGroupDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreatePipelineLayout(uint32_t, const gpu::PipelineLayoutDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateRenderPipeline(uint32_t, const gpu::RenderPipelineDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateComputePipeline(uint32_t, const gpu::ComputePipelineDescriptor&) override {
    return gpu::OkStatus();
  }
  void onDestroyResource(std::string_view, uint32_t) override {}
  gpu::Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override {
    return gpu::OkStatus();
  }
  gpu::Status onWriteTexture(uint32_t, std::span<const uint8_t>, const gpu::TexelCopyBufferLayout&,
                             const gpu::Extent2d&, const gpu::Origin2d&) override {
    return gpu::OkStatus();
  }
  gpu::Status onSubmit(uint64_t, uint32_t, std::span<const gpu::Command>) override {
    return gpu::OkStatus();
  }

private:
  gpu::ShaderSourceKind kind_;
  gpu::ShaderModuleDescriptor lastDescriptor_;
};

/// One production family: the creator under test and the two artifacts it selects between.
struct FamilyCase {
  std::string_view name;                                   //!< Test case name.
  gpu::Result<gpu::ShaderModule> (*create)(gpu::Device&);  //!< Module creator under test.
  const gpu::shader::CompiledShaderView& (*wgsl)();        //!< Authored WGSL artifact.
  const gpu::shader::CompiledShaderView& (*native)();      //!< Platform-native artifact.
};

/// Prints the family name. @param os Output stream. @param family Case to print.
std::ostream& operator<<(std::ostream& os, const FamilyCase& family) {
  return os << family.name;
}

/// Every family whose module the Geode renderer creates through `GeodeShaders.h`.
const FamilyCase kFamilies[] = {
    {"SlugFill", &createSlugFillShader, &gpu::shader::programs::SlugFillShader,
     &gpu::shader::programs::SlugFillNativeShader},
    {"SlugGradient", &createSlugGradientShader, &gpu::shader::programs::SlugGradientShader,
     &gpu::shader::programs::SlugGradientNativeShader},
    {"SlugMask", &createSlugMaskShader, &gpu::shader::programs::SlugMaskShader,
     &gpu::shader::programs::SlugMaskNativeShader},
    {"ImageBlit", &createImageBlitShader, &gpu::shader::programs::ImageBlitShader,
     &gpu::shader::programs::ImageBlitNativeShader},
};

class GeodeShaderProjectionTests : public testing::TestWithParam<FamilyCase> {};

INSTANTIATE_TEST_SUITE_P(Families, GeodeShaderProjectionTests, testing::ValuesIn(kFamilies),
                         [](const testing::TestParamInfo<FamilyCase>& info) {
                           return std::string(info.param.name);
                         });

TEST_P(GeodeShaderProjectionTests, WgslDeviceReceivesTheAuthoredWgsl) {
  ProjectionCapturingDevice device(gpu::ShaderSourceKind::Wgsl);

  EXPECT_THAT(GetParam().create(device), gpu::HasResult());

  const gpu::ShaderModuleDescriptor& descriptor = device.lastDescriptor();
  EXPECT_EQ(descriptor.sourceKind, gpu::ShaderSourceKind::Wgsl);
  EXPECT_EQ(descriptor.sourceText, GetParam().wgsl().wgsl);
  EXPECT_THAT(descriptor.spirvWords, IsEmpty());
}

TEST_P(GeodeShaderProjectionTests, NativeDeviceReceivesThePlatformProjection) {
  ProjectionCapturingDevice device(kPlatformNativeKind);

  EXPECT_THAT(GetParam().create(device), gpu::HasResult());

  const gpu::ShaderModuleDescriptor& descriptor = device.lastDescriptor();
  const gpu::shader::CompiledShaderView& native = GetParam().native();
  EXPECT_EQ(descriptor.sourceKind, kPlatformNativeKind);
  if constexpr (kPlatformNativeKind == gpu::ShaderSourceKind::Msl) {
    EXPECT_THAT(native.msl, Not(IsEmpty()));
    EXPECT_EQ(descriptor.sourceText, native.msl);
    EXPECT_THAT(descriptor.spirvWords, IsEmpty());
  } else {
    EXPECT_THAT(native.spirv, Not(IsEmpty()));
    EXPECT_THAT(descriptor.spirvWords, ElementsAreArray(native.spirv));
    EXPECT_EQ(descriptor.sourceText, std::string_view());
  }
}

TEST_P(GeodeShaderProjectionTests, SourceKindTheLinkedArtifactLacksIsRefused) {
  ProjectionCapturingDevice device(kUnlinkedNativeKind);
  const std::string expectedMessage = kUnlinkedNativeKind == gpu::ShaderSourceKind::Spirv
                                          ? "spirvWords is empty"
                                          : "sourceText is empty";

  EXPECT_THAT(
      GetParam().create(device),
      gpu::IsGpuErrorWithMessage(gpu::GpuErrorType::InvalidDescriptor, HasSubstr(expectedMessage)));
}

}  // namespace
}  // namespace donner::geode
