/// @file
/// Matrix-convolution interface, layout, and deterministic backend emission contracts.

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
#include "donner/gpu/shader/generated/ConvolveMatrixShader.h"
#include "donner/gpu/shader/programs/ConvolveMatrix.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::shader {
namespace {

std::string EmitConvolveMatrixWgsl() {
  ShaderResult<IrModule> module = programs::BuildConvolveMatrixModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitConvolveMatrixMsl() {
  ShaderResult<IrModule> module = programs::BuildConvolveMatrixModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::vector<uint32_t> EmitConvolveMatrixSpirv() {
  ShaderResult<IrModule> module = programs::BuildConvolveMatrixModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

TEST(ConvolveMatrixProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildConvolveMatrixModule(), HasShaderResult());
}

TEST(ConvolveMatrixProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitConvolveMatrixWgsl(), testing::Eq(EmitConvolveMatrixWgsl()));
  EXPECT_THAT(EmitConvolveMatrixMsl(), testing::Eq(EmitConvolveMatrixMsl()));
  EXPECT_THAT(EmitConvolveMatrixSpirv(), testing::Eq(EmitConvolveMatrixSpirv()));
}

TEST(ConvolveMatrixProgramTests, GeneratedDescriptorsPreserveTheTypedInterface) {
  ShaderResult<IrModule> module = programs::BuildConvolveMatrixModule();
  ASSERT_THAT(module, HasShaderResult());
  const std::vector<ComputeEntryPointInfo> entryPoints = ComputeEntryPointsOf(module.result());
  ShaderResult<std::vector<ShaderBufferBindingInfo>> bindings = BufferBindingsOf(module.result());
  ASSERT_THAT(bindings, HasShaderResult());

  for (ShaderSourceKind kind :
       {ShaderSourceKind::Wgsl, ShaderSourceKind::Msl, ShaderSourceKind::Spirv}) {
    ShaderModuleDescriptor descriptor = gpu::generated::convolve_matrix::BuildDescriptor(kind);
    EXPECT_THAT(descriptor.sourceKind, testing::Eq(kind));
    bool available = kind == ShaderSourceKind::Wgsl;
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    available |= kind == ShaderSourceKind::Msl;
#endif
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    available |= kind == ShaderSourceKind::Spirv;
#endif
    if (!available) {
      EXPECT_THAT(descriptor.sourceText.empty(), testing::IsTrue());
      EXPECT_THAT(descriptor.spirvWords, testing::IsEmpty());
      EXPECT_THAT(descriptor.bufferBindings.has_value(), testing::IsFalse());
      RecordingDevice device;
      EXPECT_THAT(device.createShaderModule(descriptor),
                  IsGpuError(GpuErrorType::InvalidDescriptor));
      continue;
    }
    ASSERT_THAT(descriptor.bufferBindings.has_value(), testing::IsTrue());
    EXPECT_THAT(*descriptor.bufferBindings, testing::ElementsAreArray(bindings.result()));
    EXPECT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1u));
    ASSERT_THAT(entryPoints, testing::SizeIs(1u));
    EXPECT_THAT(descriptor.computeEntryPoints[0].name, testing::Eq(entryPoints[0].name));
    EXPECT_THAT(descriptor.computeEntryPoints[0].workgroupSize.x,
                testing::Eq(entryPoints[0].workgroupSize.x));
    EXPECT_THAT(descriptor.computeEntryPoints[0].workgroupSize.y,
                testing::Eq(entryPoints[0].workgroupSize.y));
    EXPECT_THAT(descriptor.computeEntryPoints[0].workgroupSize.z,
                testing::Eq(entryPoints[0].workgroupSize.z));
    EXPECT_THAT(descriptor.sourceText.empty(), testing::Eq(kind == ShaderSourceKind::Spirv));
    EXPECT_THAT(descriptor.spirvWords.empty(), testing::Eq(kind != ShaderSourceKind::Spirv));
  }
}

TEST(ConvolveMatrixProgramTests, StorageLayoutMatchesTheUploadedParameterBlock) {
  ShaderResult<IrModule> module = programs::BuildConvolveMatrixModule();
  ASSERT_THAT(module, HasShaderResult());
  ASSERT_THAT(module.result().bindings(), testing::SizeIs(3u));
  const IrBinding& params = module.result().bindings()[2];
  EXPECT_THAT(params.kind, testing::Eq(BindingKind::ReadOnlyStorageBuffer));
  ShaderResult<StructLayout> layout = ComputeStructLayout(params.type, AddressSpace::Storage);
  ASSERT_THAT(layout, HasShaderResult());
  EXPECT_THAT(layout.result().sizeBytes, testing::Eq(132u));
  EXPECT_THAT(layout.result().members, testing::SizeIs(9u));
  const std::array<uint32_t, 9> offsets{0, 4, 8, 12, 16, 20, 24, 28, 32};
  for (size_t index = 0; index < offsets.size(); ++index) {
    EXPECT_THAT(layout.result().members[index].offsetBytes, testing::Eq(offsets[index]))
        << "member index " << index;
  }
}

}  // namespace
}  // namespace donner::gpu::shader
