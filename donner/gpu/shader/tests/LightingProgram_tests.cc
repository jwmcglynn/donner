/// @file
/// Lighting program interface, deterministic emission, and generated descriptor tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/generated/DiffuseLightingShader.h"
#include "donner/gpu/shader/programs/Lighting.h"
#include "donner/gpu/shader/programs/SpecularLightingSource.h"
#include "donner/gpu/shader/tests/CompiledSpecularLighting.h"
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
  return testing::AllOf(
      testing::Field("name", &ComputeEntryPointInfo::name, RcString(name)),
      testing::Field("workgroupSize", &ComputeEntryPointInfo::workgroupSize,
                     ::donner::gpu::WorkgroupSize{programs::kLightingWorkgroupSize,
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

TEST(SpecularLightingProgramTests, ReflectsStorageLayoutAndComputeSurface) {
  const auto& shader = tests::SpecularLightingAllProjections();
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(shader.entryPoints.front().name.view(), "cs_main");
  EXPECT_EQ(shader.entryPoints.front().stage, ShaderStage::Compute);
  EXPECT_EQ(shader.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{8, 8, 1}));
  ASSERT_THAT(shader.resources, testing::SizeIs(3));
  const auto* params = shader.resource("params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(params->minSizeBytes, sizeof(programs::LightingParams));
  EXPECT_EQ(params->binding, 2u);
  EXPECT_EQ(params->type, BindingType::ReadOnlyStorageBuffer);
  EXPECT_TRUE(shader.matchesMember("params", "specularExponent", 8, 4, ShaderScalarType::F32));
}
TEST(SpecularLightingProgramTests, OrdinaryAndFrozenNativeProjectionsAgree) {
  const auto parsed = wgsl::Parse(programs::kSpecularLightingSource.view());
  ASSERT_TRUE(parsed.hasResult());
  const auto& frozen = tests::SpecularLightingAllProjections();
  std::array<char, 32768> msl{};
  wgsl::TextSink text{msl.data(), uint32_t(msl.size())};
  ASSERT_TRUE(wgsl::EmitMsl(parsed.module, text).ok());
  EXPECT_EQ(text.view(), frozen.msl);
  std::array<uint32_t, 8192> words{};
  wgsl::SpirvSink binary{words.data(), uint32_t(words.size())};
  ASSERT_EQ(wgsl::EmitSpirv(parsed.module, binary).error, wgsl::SpirvEmitError::None);
  EXPECT_THAT(std::span(words.data(), binary.size), testing::ElementsAreArray(frozen.spirv));
  EXPECT_EQ(frozen.wgsl, programs::kSpecularLightingSource.view());
}
TEST(SpecularLightingProgramTests, MutationPropagatesThroughDescriptorAndBindingLayout) {
  const auto& shader = tests::SpecularLightingMutatedAllProjections();
  const auto descriptor = MakeShaderDescriptor(shader, ShaderSourceKind::Wgsl, "specular lighting");
  ASSERT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1));
  EXPECT_EQ(descriptor.computeEntryPoints.front().workgroupSize,
            (::donner::gpu::WorkgroupSize{4, 2, 1}));
  const auto layout = MakeBindingLayout(shader);
  ASSERT_THAT(layout, testing::SizeIs(3));
  EXPECT_EQ(layout.back().binding, 7u);
  ASSERT_TRUE(descriptor.bufferBindings.has_value());
  ASSERT_THAT(*descriptor.bufferBindings, testing::SizeIs(1));
  EXPECT_EQ(descriptor.bufferBindings->front().binding, 7u);
}
TEST(SpecularLightingProgramTests, PreservesZeroExponentAndMaximumColorAlpha) {
  const auto source = programs::kSpecularLightingSource.view();
  EXPECT_THAT(source, testing::HasSubstr("if ((exponent == 0f)) {\n    return 1f;"));
  EXPECT_THAT(source, testing::HasSubstr("max(red, max(green, blue))"));
}

}  // namespace
}  // namespace donner::gpu::shader
