/// @file
/// ComponentTransfer compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output. Native compiler and pixel tests validate execution.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/generated/ComponentTransferShader.h"
#include "donner/gpu/shader/programs/ComponentTransfer.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitComponentTransferWgsl() {
  ShaderResult<IrModule> module = programs::BuildComponentTransferModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitComponentTransferMsl() {
  ShaderResult<IrModule> module = programs::BuildComponentTransferModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::vector<uint32_t> EmitComponentTransferSpirvWords() {
  ShaderResult<IrModule> module = programs::BuildComponentTransferModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

TEST(ComponentTransferProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildComponentTransferModule(), HasShaderResult());
}

TEST(ComponentTransferProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitComponentTransferWgsl(), testing::Eq(EmitComponentTransferWgsl()));
  EXPECT_THAT(EmitComponentTransferMsl(), testing::Eq(EmitComponentTransferMsl()));
  EXPECT_THAT(EmitComponentTransferSpirvWords(), testing::Eq(EmitComponentTransferSpirvWords()));
}

TEST(ComponentTransferProgramTests, UsesPackedReadOnlyStorageAndFloatOutput) {
  const std::string wgsl = EmitComponentTransferWgsl();
  EXPECT_THAT(wgsl, HasSubstr("var<storage, read> params: array<f32>"));
  EXPECT_THAT(wgsl, HasSubstr("texture_storage_2d<rgba32float, write>"));
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("ComponentTransferParams")));
}

TEST(ComponentTransferProgramTests, GeneratedDescriptorsPreserveShaderInterface) {
  const auto module = programs::BuildComponentTransferModule();
  ASSERT_THAT(module, HasShaderResult());
  const auto bindings = BufferBindingsOf(module.result());
  ASSERT_THAT(bindings, HasShaderResult());
  const auto entryPoints = ComputeEntryPointsOf(module.result());
  ASSERT_THAT(entryPoints, testing::SizeIs(1));
  for (const auto kind : {ShaderSourceKind::Wgsl, ShaderSourceKind::Msl, ShaderSourceKind::Spirv}) {
    const auto descriptor = gpu::generated::component_transfer::BuildDescriptor(kind);
    EXPECT_EQ(descriptor.sourceKind, kind);
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
    EXPECT_EQ(descriptor.computeEntryPoints.front().name, entryPoints.front().name);
    EXPECT_EQ(descriptor.computeEntryPoints.front().workgroupSize.x,
              entryPoints.front().workgroupSize.x);
    EXPECT_EQ(descriptor.computeEntryPoints.front().workgroupSize.y,
              entryPoints.front().workgroupSize.y);
    EXPECT_EQ(descriptor.computeEntryPoints.front().workgroupSize.z,
              entryPoints.front().workgroupSize.z);
    EXPECT_EQ(descriptor.sourceText.empty(), kind == ShaderSourceKind::Spirv);
    EXPECT_EQ(descriptor.spirvWords.empty(), kind != ShaderSourceKind::Spirv);
  }
}

}  // namespace
}  // namespace donner::gpu::shader
