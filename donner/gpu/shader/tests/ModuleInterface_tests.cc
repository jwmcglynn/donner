/// @file
/// Shader buffer requirements derived from reachable IR resource references and memory layouts.

#include "donner/gpu/shader/ModuleInterface.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/gpu/shader/IrLayout.h"
#include "donner/gpu/shader/programs/ColorMatrix.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

namespace donner::gpu::shader {
namespace {

using testing::AllOf;
using testing::ElementsAre;
using testing::Field;
using testing::IsEmpty;

auto BufferIs(const RcString& entry, ShaderStage stage, uint32_t group, uint32_t binding,
              BindingType type, uint64_t size, uint32_t stride = 0) {
  return AllOf(
      Field("entryPoint", &ShaderBufferBindingInfo::entryPoint, entry),
      Field("stage", &ShaderBufferBindingInfo::stage, stage),
      Field("group", &ShaderBufferBindingInfo::group, group),
      Field("binding", &ShaderBufferBindingInfo::binding, binding),
      Field("type", &ShaderBufferBindingInfo::type, type),
      Field("minSizeBytes", &ShaderBufferBindingInfo::minSizeBytes, size),
      Field("runtimeArrayStrideBytes", &ShaderBufferBindingInfo::runtimeArrayStrideBytes, stride));
}

TEST(ModuleInterfaceTests, ColorMatrixReportsFixedUniformAndRuntimeArrayRequirements) {
  auto module = programs::BuildColorMatrixModule();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(
      GetShaderResultOrFail(BufferBindingsOf(module.result())),
      ElementsAre(BufferIs("cs_main", ShaderStage::Compute, 0, 2, BindingType::UniformBuffer, 80),
                  BufferIs("cs_main", ShaderStage::Compute, 0, 3,
                           BindingType::ReadOnlyStorageBuffer, 16, 16)));
}

TEST(ModuleInterfaceTests, HelperCallsAndNestedBranchesDetermineEachEntryPointsRequirements) {
  ModuleBuilder builder;
  const IrType params =
      GetShaderResultOrFail(IrType::Struct("Params", {{"color", IrType::Vec4f()}}), IrType::F32());
  ASSERT_THAT(builder.addUniformBuffer(2, 3, "params", params), IsShaderOk());
  ASSERT_THAT(builder.addUniformBuffer(0, 0, "unused", params), IsShaderOk());
  auto helper = builder.createFunction("readParams", {}, IrType::Vec4f());
  ASSERT_THAT(helper, HasShaderResult());
  auto member =
      Member(GetShaderResultOrFail(helper.result().ref("params"), LiteralF32(0)), "color");
  ASSERT_THAT(member, HasShaderResult());
  ASSERT_THAT(helper.result().returnValue(member.result()), IsShaderOk());
  ASSERT_THAT(helper.result().finish(), IsShaderOk());

  auto compute = builder.createComputeEntryPoint("usesHelper", {}, {1, 1, 1});
  ASSERT_THAT(compute, HasShaderResult());
  ASSERT_THAT(compute.result().beginIf(LiteralBool(true)), IsShaderOk());
  ASSERT_THAT(compute.result().elseBranch(), IsShaderOk());
  auto call = compute.result().callFunction("readParams", {});
  ASSERT_THAT(call, HasShaderResult());
  ASSERT_THAT(compute.result().addLet("color", call.result()), HasShaderResult());
  ASSERT_THAT(compute.result().endIf(), IsShaderOk());
  ASSERT_THAT(compute.result().finish(), IsShaderOk());

  auto empty = builder.createComputeEntryPoint("noBuffers", {}, {1, 1, 1});
  ASSERT_THAT(empty, HasShaderResult());
  ASSERT_THAT(empty.result().finish(), IsShaderOk());
  auto module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(GetShaderResultOrFail(BufferBindingsOf(module.result())),
              ElementsAre(BufferIs("usesHelper", ShaderStage::Compute, 2, 3,
                                   BindingType::UniformBuffer, 16)));
}

TEST(ModuleInterfaceTests, RuntimeVec3UsesPaddedStrideAndLoopExpressionsAreVisited) {
  ModuleBuilder builder;
  auto array = IrType::RuntimeArray(IrType::Vec3f());
  ASSERT_THAT(array, HasShaderResult());
  ASSERT_THAT(builder.addReadOnlyStorageBuffer(0, 1, "values", array.result()), IsShaderOk());
  auto entry = builder.createComputeEntryPoint("cs", {}, {1, 1, 1});
  ASSERT_THAT(entry, HasShaderResult());
  auto value =
      Index(GetShaderResultOrFail(entry.result().ref("values"), LiteralF32(0)), LiteralU32(0));
  ASSERT_THAT(value, HasShaderResult());
  auto scalar = Swizzle(value.result(), "x");
  ASSERT_THAT(scalar, HasShaderResult());
  auto counter = entry.result().beginFor("counter", scalar.result());
  ASSERT_THAT(counter, HasShaderResult());
  ASSERT_THAT(entry.result().forCondition(LiteralBool(false)), IsShaderOk());
  ASSERT_THAT(entry.result().forContinuing(counter.result(), scalar.result()), IsShaderOk());
  ASSERT_THAT(entry.result().endFor(), IsShaderOk());
  ASSERT_THAT(entry.result().finish(), IsShaderOk());
  auto module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(GetShaderResultOrFail(BufferBindingsOf(module.result())),
              ElementsAre(BufferIs("cs", ShaderStage::Compute, 0, 1,
                                   BindingType::ReadOnlyStorageBuffer, 16, 16)));
}

TEST(ModuleInterfaceTests, NoBuffersProducesKnownEmptyMetadata) {
  ModuleBuilder builder;
  auto entry = builder.createComputeEntryPoint("cs", {}, {1, 1, 1});
  ASSERT_THAT(entry, HasShaderResult());
  ASSERT_THAT(entry.result().finish(), IsShaderOk());
  auto module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(GetShaderResultOrFail(BufferBindingsOf(module.result())), IsEmpty());
}

TEST(ModuleInterfaceLayoutTests, MetadataRejectsOverflowingFixedSizesAndRuntimeStrides) {
  const IrType array =
      GetShaderResultOrFail(IrType::SizedArray(IrType::Vec4f(), 0x10000001u), IrType::F32());
  const IrType huge =
      GetShaderResultOrFail(IrType::Struct("Huge", {{"items", array}}), IrType::F32());
  const IrType runtimeArray = GetShaderResultOrFail(IrType::RuntimeArray(huge), IrType::F32());
  EXPECT_THAT(ComputeArrayStride(runtimeArray, AddressSpace::Storage),
              IsShaderError(testing::HasSubstr("32-bit")));
  for (bool runtime : {false, true}) {
    SCOPED_TRACE(runtime);
    ModuleBuilder builder;
    ASSERT_THAT(runtime ? builder.addReadOnlyStorageBuffer(0, 0, "data", runtimeArray)
                        : builder.addUniformBuffer(0, 0, "data", huge),
                IsShaderOk());
    auto entry = builder.createComputeEntryPoint("cs", {}, {1, 1, 1});
    ASSERT_THAT(entry, HasShaderResult());
    IrExpr value = GetShaderResultOrFail(entry.result().ref("data"), LiteralF32(0));
    if (runtime) {
      value = GetShaderResultOrFail(Index(value, LiteralU32(0)), LiteralF32(0));
    }
    ASSERT_THAT(entry.result().addLet("read", value), HasShaderResult());
    ASSERT_THAT(entry.result().finish(), IsShaderOk());
    auto module = builder.build();
    ASSERT_THAT(module, HasShaderResult());
    EXPECT_THAT(BufferBindingsOf(module.result()), IsShaderError(testing::HasSubstr("32-bit")));
  }
}

}  // namespace
}  // namespace donner::gpu::shader
