/// @file
/// Displacement-map interface and deterministic backend emission contracts.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/generated/DisplacementMapShader.h"
#include "donner/gpu/shader/programs/DisplacementMap.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::shader {
namespace {

TEST(DisplacementMapProgramTests, EmitsDeterministically) {
  const auto module = programs::BuildDisplacementMapModule();
  ASSERT_THAT(module, HasShaderResult());
  const auto wgsl = EmitWgsl(module.result());
  const auto repeatedWgsl = EmitWgsl(module.result());
  ASSERT_THAT(wgsl, HasShaderResult());
  ASSERT_THAT(repeatedWgsl, HasShaderResult());
  EXPECT_THAT(repeatedWgsl.result(), testing::Eq(wgsl.result()));
  const auto msl = EmitMsl(module.result());
  const auto repeatedMsl = EmitMsl(module.result());
  ASSERT_THAT(msl, HasShaderResult());
  ASSERT_THAT(repeatedMsl, HasShaderResult());
  EXPECT_THAT(repeatedMsl.result(), testing::Eq(msl.result()));
  const auto spirv = EmitSpirv(module.result());
  const auto repeatedSpirv = EmitSpirv(module.result());
  ASSERT_THAT(spirv, HasShaderResult());
  ASSERT_THAT(repeatedSpirv, HasShaderResult());
  EXPECT_THAT(repeatedSpirv.result(), testing::Eq(spirv.result()));
}

TEST(DisplacementMapProgramTests, GeneratedDescriptorsPreserveShaderInterface) {
  const auto module = programs::BuildDisplacementMapModule();
  ASSERT_THAT(module, HasShaderResult());
  const auto bindings = BufferBindingsOf(module.result());
  ASSERT_THAT(bindings, HasShaderResult());
  const auto entryPoints = ComputeEntryPointsOf(module.result());
  ASSERT_THAT(entryPoints, testing::SizeIs(1));
  for (const auto kind : {ShaderSourceKind::Wgsl, ShaderSourceKind::Msl, ShaderSourceKind::Spirv}) {
    const auto descriptor = gpu::generated::displacement_map::BuildDescriptor(kind);
    EXPECT_THAT(descriptor.sourceKind, testing::Eq(kind));
    bool available = kind == ShaderSourceKind::Wgsl;
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    available |= kind == ShaderSourceKind::Msl;
#endif
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    available |= kind == ShaderSourceKind::Spirv;
#endif
    if (!available) {
      EXPECT_THAT(descriptor.sourceText, testing::IsEmpty());
      EXPECT_THAT(descriptor.spirvWords, testing::IsEmpty());
      EXPECT_THAT(descriptor.bufferBindings, testing::Eq(std::nullopt));
      RecordingDevice device;
      EXPECT_THAT(device.createShaderModule(descriptor),
                  ::donner::gpu::IsGpuError(GpuErrorType::InvalidDescriptor));
      continue;
    }
    ASSERT_THAT(descriptor.bufferBindings, testing::Optional(testing::_));
    EXPECT_THAT(*descriptor.bufferBindings, testing::ElementsAreArray(bindings.result()));
    ASSERT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1));
    EXPECT_THAT(descriptor.computeEntryPoints.front().name, testing::Eq(entryPoints.front().name));
    EXPECT_THAT(descriptor.computeEntryPoints.front().workgroupSize.x,
                testing::Eq(entryPoints.front().workgroupSize.x));
    EXPECT_THAT(descriptor.computeEntryPoints.front().workgroupSize.y,
                testing::Eq(entryPoints.front().workgroupSize.y));
    EXPECT_THAT(descriptor.computeEntryPoints.front().workgroupSize.z,
                testing::Eq(entryPoints.front().workgroupSize.z));
    EXPECT_THAT(descriptor.sourceText.empty(), testing::Eq(kind == ShaderSourceKind::Spirv));
    EXPECT_THAT(descriptor.spirvWords.empty(), testing::Eq(kind != ShaderSourceKind::Spirv));
  }
}

}  // namespace
}  // namespace donner::gpu::shader
