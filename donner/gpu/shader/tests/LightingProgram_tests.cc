/// @file
/// Lighting program interface, deterministic emission, and generated descriptor tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/generated/DiffuseLightingShader.h"
#include "donner/gpu/shader/generated/SpecularLightingShader.h"
#include "donner/gpu/shader/programs/Lighting.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::shader {
namespace {

using ModuleBuilderFunction = ShaderResult<IrModule> (*)();
using DescriptorBuilderFunction = ShaderModuleDescriptor (*)(ShaderSourceKind);

struct LightingProgramCase {
  const char* name;
  ModuleBuilderFunction buildModule;
  DescriptorBuilderFunction buildDescriptor;
};

constexpr LightingProgramCase kPrograms[] = {
    {"diffuse", &programs::BuildDiffuseLightingModule,
     &gpu::generated::diffuse_lighting::BuildDescriptor},
    {"specular", &programs::BuildSpecularLightingModule,
     &gpu::generated::specular_lighting::BuildDescriptor},
};

bool IsArtifactAvailable(ShaderSourceKind kind) {
  bool available = kind == ShaderSourceKind::Wgsl;
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  available |= kind == ShaderSourceKind::Msl;
#endif
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  available |= kind == ShaderSourceKind::Spirv;
#endif
  return available;
}

auto EntryPointIs(std::string_view name) {
  return testing::AllOf(testing::Field("name", &ComputeEntryPointInfo::name, RcString(name)),
                        testing::Field("workgroupSize", &ComputeEntryPointInfo::workgroupSize,
                                       WorkgroupSize{programs::kLightingWorkgroupSize,
                                                     programs::kLightingWorkgroupSize, 1}));
}

TEST(LightingProgramTests, ModulesBuildAndEmitDeterministically) {
  for (const LightingProgramCase& program : kPrograms) {
    SCOPED_TRACE(program.name);
    ShaderResult<IrModule> module = program.buildModule();
    ASSERT_THAT(module, HasShaderResult());
    EXPECT_THAT(GetShaderResultOrFail(EmitWgsl(module.result()), std::string()),
                testing::Eq(GetShaderResultOrFail(EmitWgsl(module.result()), std::string())));
    EXPECT_THAT(GetShaderResultOrFail(EmitMsl(module.result()), std::string()),
                testing::Eq(GetShaderResultOrFail(EmitMsl(module.result()), std::string())));
    EXPECT_THAT(
        GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()),
        testing::Eq(GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>())));
  }
}

TEST(LightingProgramTests, InterfaceHasOneComputeEntryAndBoundedStorageParams) {
  for (const LightingProgramCase& program : kPrograms) {
    SCOPED_TRACE(program.name);
    ShaderResult<IrModule> module = program.buildModule();
    ASSERT_THAT(module, HasShaderResult());
    EXPECT_THAT(ComputeEntryPointsOf(module.result()),
                testing::ElementsAre(EntryPointIs(programs::kLightingEntryPoint)));
    ShaderResult<std::vector<ShaderBufferBindingInfo>> bindings = BufferBindingsOf(module.result());
    ASSERT_THAT(bindings, HasShaderResult());
    ASSERT_THAT(bindings.result(), testing::SizeIs(1));
    const ShaderBufferBindingInfo& params = bindings.result().front();
    EXPECT_THAT(params.entryPoint, testing::Eq(programs::kLightingEntryPoint));
    EXPECT_THAT(params.stage, testing::Eq(ShaderStage::Compute));
    EXPECT_THAT(params.group, testing::Eq(0u));
    EXPECT_THAT(params.binding,
                testing::Eq(static_cast<uint32_t>(programs::LightingBinding::Params)));
    EXPECT_THAT(params.type, testing::Eq(BindingType::ReadOnlyStorageBuffer));
    EXPECT_THAT(params.minSizeBytes, testing::Eq(programs::kLightingParamsSize));
    EXPECT_THAT(params.runtimeArrayStrideBytes, testing::Eq(0u));
  }
}

TEST(LightingProgramTests, GeneratedDescriptorsPreserveBackendGuardsAndInterface) {
  for (const LightingProgramCase& program : kPrograms) {
    SCOPED_TRACE(program.name);
    ShaderResult<IrModule> module = program.buildModule();
    ASSERT_THAT(module, HasShaderResult());
    ShaderResult<std::vector<ShaderBufferBindingInfo>> bindings = BufferBindingsOf(module.result());
    ASSERT_THAT(bindings, HasShaderResult());
    for (ShaderSourceKind kind :
         {ShaderSourceKind::Wgsl, ShaderSourceKind::Msl, ShaderSourceKind::Spirv}) {
      SCOPED_TRACE(kind);
      ShaderModuleDescriptor descriptor = program.buildDescriptor(kind);
      EXPECT_THAT(descriptor.sourceKind, testing::Eq(kind));
      if (!IsArtifactAvailable(kind)) {
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
      EXPECT_THAT(descriptor.computeEntryPoints,
                  testing::ElementsAre(EntryPointIs(programs::kLightingEntryPoint)));
      EXPECT_THAT(descriptor.sourceText.empty(), testing::Eq(kind == ShaderSourceKind::Spirv));
      EXPECT_THAT(descriptor.spirvWords.empty(), testing::Eq(kind != ShaderSourceKind::Spirv));
    }
  }
}

}  // namespace
}  // namespace donner::gpu::shader
