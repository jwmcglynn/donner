/// @file
/// Image-filter program emission, generated-descriptor, and layout contracts.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/IrLayout.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/generated/FilterImageShader.h"
#include "donner/gpu/shader/programs/FilterImage.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::shader {
namespace {

std::string EmitFilterImageWgsl() {
  ShaderResult<IrModule> module = programs::BuildFilterImageModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitFilterImageMsl() {
  ShaderResult<IrModule> module = programs::BuildFilterImageModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::vector<uint32_t> EmitFilterImageSpirv() {
  ShaderResult<IrModule> module = programs::BuildFilterImageModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

bool GeneratedSourceIsAvailable(ShaderSourceKind kind) {
  bool available = kind == ShaderSourceKind::Wgsl;
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  available |= kind == ShaderSourceKind::Msl;
#endif
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  available |= kind == ShaderSourceKind::Spirv;
#endif
  return available;
}

TEST(FilterImageProgramTests, ModuleBuildsAndEmitsDeterministically) {
  EXPECT_THAT(programs::BuildFilterImageModule(), HasShaderResult());
  EXPECT_THAT(EmitFilterImageWgsl(), testing::Eq(EmitFilterImageWgsl()));
  EXPECT_THAT(EmitFilterImageMsl(), testing::Eq(EmitFilterImageMsl()));
  EXPECT_THAT(EmitFilterImageSpirv(), testing::Eq(EmitFilterImageSpirv()));
}

TEST(FilterImageProgramTests, GeneratedDescriptorsPreserveTheComputedInterface) {
  ShaderResult<IrModule> module = programs::BuildFilterImageModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::vector<ShaderBufferBindingInfo>> bindings = BufferBindingsOf(module.result());
  ASSERT_THAT(bindings, HasShaderResult());

  for (ShaderSourceKind kind :
       {ShaderSourceKind::Wgsl, ShaderSourceKind::Msl, ShaderSourceKind::Spirv}) {
    SCOPED_TRACE(testing::Message() << kind);
    ShaderModuleDescriptor descriptor = gpu::generated::filter_image::BuildDescriptor(kind);
    EXPECT_THAT(descriptor.sourceKind, testing::Eq(kind));
    if (!GeneratedSourceIsAvailable(kind)) {
      EXPECT_THAT(descriptor.sourceText, testing::IsEmpty());
      EXPECT_THAT(descriptor.spirvWords, testing::IsEmpty());
      EXPECT_THAT(descriptor.bufferBindings, testing::Eq(std::nullopt));
      EXPECT_THAT(descriptor.computeEntryPoints, testing::IsEmpty());
      RecordingDevice device;
      EXPECT_THAT(device.createShaderModule(descriptor),
                  IsGpuError(GpuErrorType::InvalidDescriptor));
      continue;
    }

    ASSERT_THAT(descriptor.bufferBindings, testing::Optional(testing::_));
    EXPECT_THAT(*descriptor.bufferBindings, testing::ElementsAreArray(bindings.result()));
    ASSERT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1u));
    EXPECT_THAT(descriptor.computeEntryPoints.front().name,
                testing::Eq(programs::kFilterImageEntryPoint));
    EXPECT_THAT(descriptor.computeEntryPoints.front().workgroupSize,
                testing::Eq(WorkgroupSize{programs::kFilterImageWorkgroupSize,
                                          programs::kFilterImageWorkgroupSize, 1}));
    EXPECT_THAT(descriptor.sourceText.empty(), testing::Eq(kind == ShaderSourceKind::Spirv));
    EXPECT_THAT(descriptor.spirvWords.empty(), testing::Eq(kind != ShaderSourceKind::Spirv));
  }
}

TEST(FilterImageProgramTests, UniformLayoutMatchesTheFortyByteHostBlock) {
  ShaderResult<IrModule> module = programs::BuildFilterImageModule();
  ASSERT_THAT(module, HasShaderResult());
  ASSERT_THAT(module.result().bindings(), testing::SizeIs(3u));
  const IrBinding& binding = module.result().bindings().back();
  EXPECT_THAT(binding.group, testing::Eq(0u));
  EXPECT_THAT(binding.binding,
              testing::Eq(static_cast<uint32_t>(programs::FilterImageBinding::Params)));
  EXPECT_THAT(binding.kind, testing::Eq(BindingKind::UniformBuffer));

  ShaderResult<StructLayout> layout = ComputeStructLayout(binding.type, AddressSpace::Uniform);
  ASSERT_THAT(layout, HasShaderResult());
  EXPECT_THAT(layout.result().sizeBytes, testing::Eq(40u));
  ASSERT_THAT(layout.result().members, testing::SizeIs(10u));
  const std::array<uint32_t, 10> expectedOffsets{0, 4, 8, 12, 16, 20, 24, 28, 32, 36};
  for (size_t index = 0; index < expectedOffsets.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_THAT(layout.result().members[index].offsetBytes, testing::Eq(expectedOffsets[index]));
  }
}

TEST(FilterImageProgramTests, WgslDeclaresFloatStorageAndAllSamplingPaths) {
  const std::string wgsl = EmitFilterImageWgsl();
  EXPECT_THAT(wgsl, testing::HasSubstr("@compute @workgroup_size(8, 8, 1)\nfn cs_main("));
  EXPECT_THAT(wgsl, testing::HasSubstr("@group(0) @binding(0) var imageTexture: texture_2d<f32>;"));
  EXPECT_THAT(wgsl, testing::HasSubstr("texture_storage_2d<rgba32float, write>"));
  EXPECT_THAT(wgsl, testing::HasSubstr("var<uniform> params: ImageParams;"));
  EXPECT_THAT(wgsl, testing::HasSubstr("fn sampleImage("));
  EXPECT_THAT(wgsl, testing::HasSubstr("fn samplePixelated("));
  EXPECT_THAT(wgsl, testing::HasSubstr("fn sampleSmooth("));
}

}  // namespace
}  // namespace donner::gpu::shader
