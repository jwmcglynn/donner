/// @file
/// Turbulence interface, table layout, and deterministic backend emission contracts.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/IrLayout.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/generated/TurbulenceShader.h"
#include "donner/gpu/shader/programs/Turbulence.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::shader {
namespace {

TEST(TurbulenceProgramTests, EmitsDeterministically) {
  const auto module = programs::BuildTurbulenceModule();
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

TEST(TurbulenceProgramTests, StorageLayoutsMatchHostParameterAndTableBytes) {
  const auto module = programs::BuildTurbulenceModule();
  ASSERT_THAT(module, HasShaderResult());
  const auto paramsBinding =
      std::find_if(module.result().bindings().begin(), module.result().bindings().end(),
                   [](const IrBinding& value) { return value.binding == 1; });
  ASSERT_THAT(paramsBinding, testing::Ne(module.result().bindings().end()));
  const auto params = ComputeStructLayout(paramsBinding->type, AddressSpace::Storage);
  ASSERT_THAT(params, HasShaderResult());
  EXPECT_THAT(params.result().sizeBytes, testing::Eq(48u));
  ASSERT_THAT(params.result().members, testing::SizeIs(12));
  for (size_t index = 0; index < params.result().members.size(); ++index) {
    EXPECT_THAT(params.result().members[index].offsetBytes, testing::Eq(index * 4));
  }
  const auto tablesBinding =
      std::find_if(module.result().bindings().begin(), module.result().bindings().end(),
                   [](const IrBinding& value) { return value.binding == 2; });
  ASSERT_THAT(tablesBinding, testing::Ne(module.result().bindings().end()));
  const auto tables = ComputeStructLayout(tablesBinding->type, AddressSpace::Storage);
  ASSERT_THAT(tables, HasShaderResult());
  EXPECT_THAT(tables.result().sizeBytes, testing::Eq(18504u));
  ASSERT_THAT(tables.result().members, testing::SizeIs(3));
  EXPECT_THAT(tables.result().members[0].offsetBytes, testing::Eq(0u));
  EXPECT_THAT(tables.result().members[1].offsetBytes, testing::Eq(2056u));
  EXPECT_THAT(tables.result().members[2].offsetBytes, testing::Eq(10280u));
}

TEST(TurbulenceProgramTests, GeneratedDescriptorsPreserveShaderInterface) {
  const auto module = programs::BuildTurbulenceModule();
  ASSERT_THAT(module, HasShaderResult());
  const auto bindings = BufferBindingsOf(module.result());
  ASSERT_THAT(bindings, HasShaderResult());
  const auto entryPoints = ComputeEntryPointsOf(module.result());
  ASSERT_THAT(entryPoints, testing::SizeIs(1));
  for (const auto kind : {ShaderSourceKind::Wgsl, ShaderSourceKind::Msl, ShaderSourceKind::Spirv}) {
    const auto descriptor = gpu::generated::turbulence::BuildDescriptor(kind);
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
    EXPECT_THAT(descriptor.computeEntryPoints.front().workgroupSize,
                testing::Eq(entryPoints.front().workgroupSize));
    EXPECT_THAT(descriptor.sourceText.empty(), testing::Eq(kind == ShaderSourceKind::Spirv));
    EXPECT_THAT(descriptor.spirvWords.empty(), testing::Eq(kind != ShaderSourceKind::Spirv));
  }
}

}  // namespace
}  // namespace donner::gpu::shader
