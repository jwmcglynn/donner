/// @file
/// Checkerboard interface and backend emission contracts.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/IrLayout.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/generated/CheckerboardShader.h"
#include "donner/gpu/shader/programs/Checkerboard.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::shader {

TEST(CheckerboardProgramTests, GeneratedDescriptorsPreserveShaderInterface) {
  auto module = programs::BuildCheckerboardModule();
  ASSERT_THAT(module, HasShaderResult());
  auto bindings = BufferBindingsOf(module.result());
  ASSERT_THAT(bindings, HasShaderResult());
  for (auto kind : {ShaderSourceKind::Wgsl, ShaderSourceKind::Msl, ShaderSourceKind::Spirv}) {
    auto descriptor = gpu::generated::checkerboard::BuildDescriptor(kind);
    EXPECT_EQ(descriptor.sourceKind, kind);
    bool available = kind == ShaderSourceKind::Wgsl;
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    available |= kind == ShaderSourceKind::Msl;
#endif
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    available |= kind == ShaderSourceKind::Spirv;
#endif
    if (!available) {
      EXPECT_TRUE(descriptor.sourceText.empty());
      EXPECT_THAT(descriptor.spirvWords, testing::IsEmpty());
      EXPECT_FALSE(descriptor.bufferBindings.has_value());
      RecordingDevice device;
      EXPECT_THAT(device.createShaderModule(descriptor),
                  IsGpuError(GpuErrorType::InvalidDescriptor));
      continue;
    }
    ASSERT_TRUE(descriptor.bufferBindings.has_value());
    EXPECT_THAT(*descriptor.bufferBindings, testing::ElementsAreArray(bindings.result()));
    EXPECT_THAT(descriptor.computeEntryPoints, testing::IsEmpty());
    EXPECT_EQ(descriptor.sourceText.empty(), kind == ShaderSourceKind::Spirv);
    EXPECT_EQ(descriptor.spirvWords.empty(), kind != ShaderSourceKind::Spirv);
  }
}

TEST(CheckerboardProgramTests, UniformLayoutMatchesTheUploadedSixtyFourBytes) {
  auto module = programs::BuildCheckerboardModule();
  ASSERT_THAT(module, HasShaderResult());
  ASSERT_EQ(module.result().bindings().size(), 1u);
  const IrBinding& binding = module.result().bindings().front();
  EXPECT_EQ(binding.group, 0u);
  EXPECT_EQ(binding.binding, 0u);
  EXPECT_EQ(binding.kind, BindingKind::UniformBuffer);
  auto layout = ComputeStructLayout(binding.type, AddressSpace::Uniform);
  ASSERT_THAT(layout, HasShaderResult());
  EXPECT_EQ(layout.result().sizeBytes, 64u);
  const std::array<uint32_t, 7> offsets{0, 8, 12, 16, 32, 48, 56};
  ASSERT_EQ(layout.result().members.size(), offsets.size());
  for (size_t i = 0; i < offsets.size(); ++i) {
    EXPECT_EQ(layout.result().members[i].offsetBytes, offsets[i]);
  }
}

TEST(CheckerboardProgramTests, AllBackendEmittersAcceptTheRenderModule) {
  auto module = programs::BuildCheckerboardModule();
  ASSERT_THAT(module, HasShaderResult());
  ASSERT_EQ(module.result().functions().size(), 2u);
  EXPECT_THAT(EmitWgsl(module.result()), HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()), HasShaderResult());
  EXPECT_THAT(EmitSpirv(module.result()), HasShaderResult());
}

}  // namespace donner::gpu::shader
