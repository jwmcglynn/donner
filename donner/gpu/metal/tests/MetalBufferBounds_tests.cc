/// @file
/// Declared buffer ranges remain separate from the containing allocation.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/benchmarks/AllocationTracker.h"

namespace donner::gpu::metal {
namespace {

shader::ShaderResult<shader::IrModule> BuildBufferReadModule() {
  using namespace shader;
  programs::ErrorLatch e;
  ModuleBuilder builder;
  const IrType params = e(IrType::Struct("Params", {{"index", IrType::I32()}}));
  e.ok(builder.addReadOnlyStorageBuffer(0, 0, "values", e(IrType::RuntimeArray(IrType::Vec4f()))));
  e.ok(builder.addUniformBuffer(0, 1, "params", params));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, 2, "outputTexture",
                                            StorageTextureFormat::Rgba8Unorm));
  auto helperResult =
      builder.createFunction("readValue", {{"index", IrType::I32()}}, IrType::Vec4f());
  if (helperResult.hasError()) {
    return std::move(helperResult).error();
  }
  FunctionBuilder helper = std::move(helperResult).result();
  e.ok(helper.returnValue(e(Index(e(helper.ref("values")), e(helper.ref("index"))))));
  e.ok(helper.finish());
  auto entryResult = builder.createComputeEntryPoint("cs", {}, {1, 1, 1});
  if (entryResult.hasError()) {
    return std::move(entryResult).error();
  }
  FunctionBuilder entry = std::move(entryResult).result();
  const IrExpr value =
      e(entry.callFunction("readValue", {e(Member(e(entry.ref("params")), "index"))}));
  e.ok(entry.textureStore(e(entry.ref("outputTexture")),
                          e(ConstructVector(IrType::Vec2i(), {LiteralI32(0)})), value));
  e.ok(entry.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

enum class BindingOrder { Normal, BeforePipeline, PipelineSwitch, Replacement };

struct ReadOptions {
  BindingOrder bindingOrder = BindingOrder::Normal;
  uint32_t dispatchCount = 1;
  bool bindEachDispatch = false;
  bool secondPass = false;
  /// Declare the runtime-array binding as a uniform buffer in the bind group layout, while the
  /// shader IR still reads it as a runtime array. The RHI permits this: the buffer carries both
  /// usages and the layout's binding type is a separate fact from the IR's.
  bool declareRuntimeArrayAsUniform = false;
};

class MetalBufferBoundsTests : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(device_, "Metal declared buffer bounds");
    auto ir = BuildBufferReadModule();
    ASSERT_FALSE(ir.hasError()) << ir.error();
    auto msl = shader::EmitMsl(ir.result());
    ASSERT_FALSE(msl.hasError()) << msl.error();
    auto facts = shader::BufferBindingsOf(ir.result());
    ASSERT_FALSE(facts.hasError()) << facts.error();
    module_ =
        GetResultOrFail(device_->createShaderModule({"readBuffer",
                                                     RcString(msl.result()),
                                                     ShaderSourceKind::Msl,
                                                     {},
                                                     shader::ComputeEntryPointsOf(ir.result()),
                                                     std::move(facts).result()}));
    groupLayout_ = GetResultOrFail(device_->createBindGroupLayout(
        {"readBuffer",
         {{0, ShaderStage::Compute, BindingType::ReadOnlyStorageBuffer},
          {1, ShaderStage::Compute, BindingType::UniformBuffer},
          {2, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d}}}));
    layout_ = GetResultOrFail(device_->createPipelineLayout({"readBuffer", {groupLayout_}}));
    // The same generated MSL with its interface metadata omitted, which the backend documents as
    // retaining the shared runtime's existing semantics. Without facts, nothing cross-checks the
    // layout's binding types against the IR, so the layout may name the runtime array a uniform
    // buffer while the generated code still indexes it through donner_msl_lengths.
    noFactsModule_ =
        GetResultOrFail(device_->createShaderModule({"readBufferNoFacts",
                                                     RcString(msl.result()),
                                                     ShaderSourceKind::Msl,
                                                     {},
                                                     shader::ComputeEntryPointsOf(ir.result()),
                                                     std::nullopt}));
    uniformDeclaredGroupLayout_ = GetResultOrFail(device_->createBindGroupLayout(
        {"readBufferAsUniform",
         {{0, ShaderStage::Compute, BindingType::UniformBuffer},
          {1, ShaderStage::Compute, BindingType::UniformBuffer},
          {2, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d}}}));
    uniformDeclaredLayout_ = GetResultOrFail(
        device_->createPipelineLayout({"readBufferAsUniform", {uniformDeclaredGroupLayout_}}));
    pipeline_ = GetResultOrFail(
        device_->createComputePipeline({"readBuffer", layout_, {module_, "cs"}, {1, 1, 1}}));
    alternatePipeline_ = GetResultOrFail(
        device_->createComputePipeline({"alternate", layout_, {module_, "cs"}, {1, 1, 1}}));
    uniformDeclaredPipeline_ = GetResultOrFail(device_->createComputePipeline(
        {"readBufferAsUniform", uniformDeclaredLayout_, {noFactsModule_, "cs"}, {1, 1, 1}}));
  }

  void bindForRead(ComputePassEncoder* pass, const BindGroup& group, const BindGroup& fullGroup,
                   BindingOrder order, const ComputePipeline& pipeline) {
    if (order == BindingOrder::BeforePipeline) {
      ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
      ASSERT_THAT(pass->setPipeline(pipeline), IsOk());
      return;
    }
    ASSERT_THAT(pass->setPipeline(pipeline), IsOk());
    if (order == BindingOrder::Replacement) {
      ASSERT_THAT(pass->setBindGroup(0, fullGroup), IsOk());
      ASSERT_THAT(pass->dispatchWorkgroups(1), IsOk());
    }
    ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
    if (order == BindingOrder::PipelineSwitch) {
      ASSERT_THAT(pass->setPipeline(alternatePipeline_), IsOk());
    }
  }

  void expectRead(int32_t index, uint64_t offset, uint64_t declaredBytes,
                  const std::array<uint8_t, 4>& expected, ReadOptions options = {}) {
    const std::array<float, 8> values{1, 0, 0, 1, 0, 1, 0, 1};
    std::vector<uint8_t> data(offset + sizeof(values));
    std::memcpy(data.data() + offset, values.data(), sizeof(values));
    const ComputePipeline& selectedPipeline =
        options.declareRuntimeArrayAsUniform ? uniformDeclaredPipeline_ : pipeline_;
    const BindGroupLayout& selectedGroupLayout =
        options.declareRuntimeArrayAsUniform ? uniformDeclaredGroupLayout_ : groupLayout_;
    const Buffer input = GetResultOrFail(device_->createBuffer(
        {"values", data.size(),
         BufferUsage::Storage | BufferUsage::Uniform | BufferUsage::CopyDst}));
    ASSERT_THAT(device_->writeBuffer(input, 0, data), IsOk());
    const Buffer params = GetResultOrFail(device_->createBuffer(
        {"params", sizeof(index), BufferUsage::Uniform | BufferUsage::CopyDst}));
    ASSERT_THAT(device_->writeBuffer(params, 0,
                                     std::span<const uint8_t>(
                                         reinterpret_cast<const uint8_t*>(&index), sizeof(index))),
                IsOk());
    const Texture output = GetResultOrFail(
        device_->createTexture({"output",
                                {1, 1},
                                TextureFormat::RGBA8Unorm,
                                TextureUsage::StorageBinding | TextureUsage::CopySrc}));
    const TextureView view = GetResultOrFail(device_->createTextureView(output, {}));
    const Buffer readback = GetResultOrFail(
        device_->createBuffer({"readback", 256, BufferUsage::CopyDst | BufferUsage::MapRead}));
    BindGroupDescriptor groupDescriptor{"readBuffer",
                                        selectedGroupLayout,
                                        {{0, BufferBinding{input, offset, declaredBytes}},
                                         {1, BufferBinding{params, 0, sizeof(index)}},
                                         {2, TextureViewBinding{view}}}};
    const BindGroup group = GetResultOrFail(device_->createBindGroup(groupDescriptor));
    std::get<BufferBinding>(groupDescriptor.entries[0].resource).sizeBytes = sizeof(values);
    const BindGroup fullGroup = GetResultOrFail(device_->createBindGroup(groupDescriptor));
    auto encoder = GetResultOrFail(device_->createCommandEncoder());
    ComputePassEncoder* pass = GetResultOrFail(encoder->beginComputePass({}));
    bindForRead(pass, group, fullGroup, options.bindingOrder, selectedPipeline);
    for (uint32_t dispatch = 0; dispatch < options.dispatchCount; ++dispatch) {
      if (options.bindEachDispatch && dispatch != 0) {
        ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
      }
      ASSERT_THAT(pass->dispatchWorkgroups(1), IsOk());
    }
    ASSERT_THAT(pass->end(), IsOk());
    if (options.secondPass) {
      pass = GetResultOrFail(encoder->beginComputePass({}));
      ASSERT_THAT(pass->setPipeline(selectedPipeline), IsOk());
      ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
      ASSERT_THAT(pass->dispatchWorkgroups(1), IsOk());
      ASSERT_THAT(pass->end(), IsOk());
    }
    ASSERT_THAT(
        encoder->copyTextureToBuffer(TexelCopyTextureInfo{output}, readback, {0, 256, 1}, {1, 1}),
        IsOk());
    CommandBuffer commands = GetResultOrFail(encoder->finish());
    benchmarks::allocations::Scope allocations;
    const auto start = std::chrono::steady_clock::now();
    const uint64_t serial = GetResultOrFail(device_->submit(std::move(commands)));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto snapshot = allocations.stop();
    if (options.dispatchCount > 1) {
      std::fprintf(
          stderr,
          "native_dispatch rebind=%d count=%u ns=%.3f allocations=%" PRIu64 " bytes=%" PRIu64 "\n",
          options.bindEachDispatch, options.dispatchCount,
          std::chrono::duration<double, std::nano>(elapsed).count() / options.dispatchCount,
          snapshot.allocationCalls, snapshot.allocationBytes);
    }
    ASSERT_THAT(device_->waitForSerial(serial, 5.0), testing::IsTrue())
        << device_->lastErrorForTest();
    auto result = device_->readBackBuffer(readback);
    ASSERT_THAT(result, HasResult());
    ASSERT_GE(result.result().size(), 4u);
    result.result().resize(4);
    const svg::RendererBitmap actual{
        .dimensions = {1, 1}, .pixels = std::move(result).result(), .rowBytes = 4};
    const svg::RendererBitmap reference{
        .dimensions = {1, 1},
        .pixels = std::vector<uint8_t>(expected.begin(), expected.end()),
        .rowBytes = 4};
    editor::tests::CompareBitmapToBitmap(actual, reference,
                                         std::format("buffer_range_{}_{}", offset, index),
                                         editor::tests::PixelmatchIdentityParams());
  }

  std::unique_ptr<MetalDevice> device_;
  ShaderModule module_;
  ShaderModule noFactsModule_;
  BindGroupLayout groupLayout_;
  PipelineLayout layout_;
  ComputePipeline pipeline_;
  ComputePipeline alternatePipeline_;
  BindGroupLayout uniformDeclaredGroupLayout_;
  PipelineLayout uniformDeclaredLayout_;
  ComputePipeline uniformDeclaredPipeline_;
};

TEST_F(MetalBufferBoundsTests, RejectsBindingsBeyondTheNativeArgumentTables) {
  EXPECT_THAT(device_->createBindGroupLayout(
                  {"buffer limit", {{29, ShaderStage::Compute, BindingType::UniformBuffer}}}),
              IsGpuError(GpuErrorType::Unsupported));
  EXPECT_THAT(device_->createBindGroupLayout(
                  {"sampler limit", {{16, ShaderStage::Fragment, BindingType::FilteringSampler}}}),
              IsGpuError(GpuErrorType::Unsupported));
}

TEST_F(MetalBufferBoundsTests, ADeclaredRangeDoesNotExposeTheRestOfTheAllocation) {
  expectRead(0, 0, 16, {255, 0, 0, 255});
  expectRead(1, 0, 16, {0, 0, 0, 0});
}

TEST_F(MetalBufferBoundsTests, AnOffsetRangeDoesNotExposeItsFollowingElement) {
  expectRead(0, 256, 16, {255, 0, 0, 255});
  expectRead(1, 256, 16, {0, 0, 0, 0});
}

TEST_F(MetalBufferBoundsTests, RangesExposeOnlyTheirWholeElements) {
  expectRead(1, 0, 31, {0, 0, 0, 0});
  expectRead(1, 0, 32, {0, 255, 0, 255});
  expectRead(1, 256, 32, {0, 255, 0, 255});
}

TEST_F(MetalBufferBoundsTests, ReplacingAGroupReplacesItsDeclaredLengths) {
  expectRead(1, 256, 16, {0, 0, 0, 0}, {.bindingOrder = BindingOrder::Replacement});
}

TEST_F(MetalBufferBoundsTests, PipelineSwitchPreservesTheBoundLengths) {
  expectRead(0, 256, 16, {255, 0, 0, 255}, {.bindingOrder = BindingOrder::PipelineSwitch});
  expectRead(1, 256, 16, {0, 0, 0, 0}, {.bindingOrder = BindingOrder::PipelineSwitch});
}

TEST_F(MetalBufferBoundsTests, GroupCanBeBoundBeforeThePipeline) {
  expectRead(0, 0, 16, {255, 0, 0, 255}, {.bindingOrder = BindingOrder::BeforePipeline});
  expectRead(1, 0, 16, {0, 0, 0, 0}, {.bindingOrder = BindingOrder::BeforePipeline});
}

TEST_F(MetalBufferBoundsTests, RejectsGroupsOutsideTheNativeMapping) {
  const Buffer buffer = GetResultOrFail(device_->createBuffer({"value", 16, BufferUsage::Uniform}));
  const BindGroupLayout layout = GetResultOrFail(device_->createBindGroupLayout(
      {"value", {{0, ShaderStage::Compute, BindingType::UniformBuffer}}}));
  const BindGroup group = GetResultOrFail(
      device_->createBindGroup({"value", layout, {{0, BufferBinding{buffer, 0, 16}}}}));
  auto encoder = GetResultOrFail(device_->createCommandEncoder());
  ComputePassEncoder* pass = GetResultOrFail(encoder->beginComputePass({}));
  ASSERT_THAT(pass->setBindGroup(1, group), IsOk());
  ASSERT_THAT(pass->end(), IsOk());
  EXPECT_THAT(device_->submit(GetResultOrFail(encoder->finish())),
              IsGpuError(GpuErrorType::Unsupported));
}

TEST_F(MetalBufferBoundsTests, LayoutBindingTypeDoesNotDecideWhetherLengthsAreBound) {
  // The emitter puts donner_msl_lengths in every entry point of a module that holds any
  // runtime-array binding, so the generated code dereferences it however the layout names the
  // binding. With interface metadata supplied, pipeline creation rejects a layout that disagrees
  // with the shader; with metadata omitted, which this backend still accepts, nothing does.
  // Uploading the table only for layout entries typed ReadOnlyStorageBuffer left that pipeline
  // reading a nil constant uint*, so the declared ranges below were never applied.
  expectRead(0, 0, 16, {255, 0, 0, 255}, {.declareRuntimeArrayAsUniform = true});
  expectRead(1, 0, 16, {0, 0, 0, 0}, {.declareRuntimeArrayAsUniform = true});
}

TEST_F(MetalBufferBoundsTests, ANewPassBindsTheSameGroupsLengthsAgain) {
  expectRead(0, 256, 16, {255, 0, 0, 255}, {.secondPass = true});
  expectRead(1, 256, 16, {0, 0, 0, 0}, {.secondPass = true});
}

TEST_F(MetalBufferBoundsTests, RepeatedBindingsReportNativeSubmissionCost) {
  for (bool bindEach : {false, true}) {
    for (uint32_t sample = 0; sample < 7; ++sample) {
      expectRead(0, 0, 16, {255, 0, 0, 255}, {.dispatchCount = 512, .bindEachDispatch = bindEach});
    }
  }
}

}  // namespace
}  // namespace donner::gpu::metal
