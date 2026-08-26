/// @file
/// Compute surface tests: compute pipeline validation, storage-texture bind group validation, the
/// compute pass state machine, dispatch prerequisites, submission re-validation, and the
/// deterministic recording of every new command.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu {
namespace {

// The runtime and the shader IR validate the same workgroup shape at two layers that cannot
// include each other's headers, so each carries its own copy of the caps. Tying them together
// here means tuning one side alone fails this build instead of producing a pipeline that passes
// one layer's check and is rejected by the other's.
static_assert(kMaxComputeInvocationsPerWorkgroup == shader::kMaxComputeInvocationsPerWorkgroup,
              "the runtime and shader IR invocation caps must stay in step");
static_assert(kMaxComputeWorkgroupSizeXY == shader::kMaxComputeWorkgroupSizeXY,
              "the runtime and shader IR X/Y workgroup caps must stay in step");
static_assert(kMaxComputeWorkgroupSizeZ == shader::kMaxComputeWorkgroupSizeZ,
              "the runtime and shader IR Z workgroup caps must stay in step");

/// Builds the color-matrix compute scene: a sampled input texture, a write-only storage output
/// texture, a uniform params buffer, a read-only storage bias buffer, and the pipeline binding
/// all four.
class ComputePassTests : public testing::Test {
protected:
  void SetUp() override {
    input_ = GetResultOrFail(device_.createTexture(
        TextureDescriptor{"input", Extent2d{16, 16}, TextureFormat::RGBA8Unorm,
                          TextureUsage::Sampled | TextureUsage::CopyDst}));
    inputView_ =
        GetResultOrFail(device_.createTextureView(input_, TextureViewDescriptor{"inputView"}));
    output_ = GetResultOrFail(device_.createTexture(
        TextureDescriptor{"output", Extent2d{16, 16}, TextureFormat::RGBA8Unorm,
                          TextureUsage::StorageBinding | TextureUsage::CopySrc}));
    outputView_ =
        GetResultOrFail(device_.createTextureView(output_, TextureViewDescriptor{"outputView"}));
    params_ = GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"params", 64, BufferUsage::Uniform | BufferUsage::CopyDst}));
    bias_ = GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"bias", 32, BufferUsage::Storage | BufferUsage::CopyDst}));

    bindGroupLayout_ = GetResultOrFail(
        device_.createBindGroupLayout(BindGroupLayoutDescriptor{"colorMatrix", layoutEntries()}));
    pipelineLayout_ = GetResultOrFail(device_.createPipelineLayout(
        PipelineLayoutDescriptor{"colorMatrixLayout", {bindGroupLayout_}}));
    bindGroup_ = GetResultOrFail(device_.createBindGroup(
        BindGroupDescriptor{"colorMatrixGroup", bindGroupLayout_, bindGroupEntries(outputView_)}));
    shader_ = GetResultOrFail(device_.createShaderModule(shaderModuleDescriptor()));
    pipeline_ = GetResultOrFail(device_.createComputePipeline(pipelineDescriptor()));

    encoder_ = GetResultOrFail(device_.createCommandEncoder());
  }

  /// The four-entry compute layout: sampled input, storage output, uniform params, storage bias.
  static std::vector<BindGroupLayoutEntry> layoutEntries() {
    return {BindGroupLayoutEntry{0, ShaderStage::Compute, BindingType::SampledTexture2dFloat},
            BindGroupLayoutEntry{1, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d,
                                 TextureFormat::RGBA8Unorm},
            BindGroupLayoutEntry{2, ShaderStage::Compute, BindingType::UniformBuffer},
            BindGroupLayoutEntry{3, ShaderStage::Compute, BindingType::ReadOnlyStorageBuffer}};
  }

  /// Bind group entries binding \p storageView as the storage output.
  /// @param storageView Texture view bound at the storage-texture binding.
  std::vector<BindGroupEntry> bindGroupEntries(const TextureView& storageView) const {
    return {BindGroupEntry{0, TextureViewBinding{inputView_}},
            BindGroupEntry{1, TextureViewBinding{storageView}},
            BindGroupEntry{2, BufferBinding{params_, 0, 64}},
            BindGroupEntry{3, BufferBinding{bias_, 0, 32}}};
  }

  /// The module the fixture compiles, declaring the one compute entry point its source carries.
  static ShaderModuleDescriptor shaderModuleDescriptor() {
    return ShaderModuleDescriptor{"colorMatrix",
                                  "@compute @workgroup_size(8, 8, 1) fn csMain() {}",
                                  ShaderSourceKind::Wgsl,
                                  {},
                                  {ComputeEntryPointInfo{"csMain", WorkgroupSize{8, 8, 1}}}};
  }

  ComputePipelineDescriptor pipelineDescriptor() const {
    return ComputePipelineDescriptor{"colorMatrix", pipelineLayout_,
                                     ComputeState{shader_, "csMain"}, WorkgroupSize{8, 8, 1}};
  }

  /// Begins the standard compute pass, failing the test if it cannot begin.
  ComputePassEncoder* beginComputePass() {
    return GetResultOrFail(encoder_->beginComputePass(ComputePassDescriptor{"colorMatrixPass"}));
  }

  RecordingDevice device_;
  Texture input_;
  TextureView inputView_;
  Texture output_;
  TextureView outputView_;
  Buffer params_;
  Buffer bias_;
  BindGroupLayout bindGroupLayout_;
  PipelineLayout pipelineLayout_;
  BindGroup bindGroup_;
  ShaderModule shader_;
  ComputePipeline pipeline_;
  std::unique_ptr<CommandEncoder> encoder_;
};

TEST_F(ComputePassTests, FullPassEncodesSubmitsAndRecordsDeterministically) {
  ComputePassEncoder* pass = beginComputePass();
  ASSERT_NE(pass, nullptr);

  EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
  EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(2, 2, 1), IsOk());
  EXPECT_THAT(pass->end(), IsOk());

  auto finished = encoder_->finish();
  ASSERT_THAT(finished, HasResult());
  EXPECT_THAT(device_.submit(std::move(finished).result()), HasResult());

  const std::string recording = device_.serialize();
  EXPECT_THAT(recording, HasSubstr("createComputePipeline computePipeline#0 label=\"colorMatrix\" "
                                   "layout=pipelineLayout#0 compute={module=shaderModule#0 "
                                   "entryPoint=\"csMain\"} workgroupSize=8x8x1"));
  EXPECT_THAT(recording, HasSubstr("  beginComputePass label=\"colorMatrixPass\""));
  EXPECT_THAT(recording, HasSubstr("  setComputePipeline computePipeline#0"));
  EXPECT_THAT(recording, HasSubstr("  dispatchWorkgroups x=2 y=2 z=1"));
  EXPECT_THAT(recording, HasSubstr("  endComputePass"));
  EXPECT_THAT(recording, HasSubstr("type=WriteOnlyStorageTexture2d storageTextureFormat="
                                   "RGBA8Unorm"));
}

TEST_F(ComputePassTests, DispatchBeforeSetPipelineFails) {
  ComputePassEncoder* pass = beginComputePass();
  EXPECT_THAT(pass->dispatchWorkgroups(1),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("no pipeline is set")));
}

TEST_F(ComputePassTests, DispatchWithoutTheRequiredBindGroupFails) {
  ComputePassEncoder* pass = beginComputePass();
  EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(1),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                    HasSubstr("requires a bind group at index 0")));
}

TEST_F(ComputePassTests, DispatchWithAMismatchedBindGroupLayoutFails) {
  const BindGroupLayout otherLayout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "other", {BindGroupLayoutEntry{0, ShaderStage::Compute, BindingType::UniformBuffer}}}));
  const BindGroup otherGroup = GetResultOrFail(device_.createBindGroup(BindGroupDescriptor{
      "otherGroup", otherLayout, {BindGroupEntry{0, BufferBinding{params_, 0, 64}}}}));

  ComputePassEncoder* pass = beginComputePass();
  EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
  EXPECT_THAT(pass->setBindGroup(0, otherGroup), IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(1),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                    HasSubstr("created against a different layout")));
}

TEST_F(ComputePassTests, DispatchRejectsZeroAndOversizedWorkgroupCounts) {
  ComputePassEncoder* pass = beginComputePass();
  EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
  EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(4, 0, 1),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("4x0x1 has a zero dimension")));

  // The first error poisons the encoder, so the limit case needs a fresh one.
  std::unique_ptr<CommandEncoder> fresh = GetResultOrFail(device_.createCommandEncoder());
  ComputePassEncoder* freshPass =
      GetResultOrFail(fresh->beginComputePass(ComputePassDescriptor{"limits"}));
  EXPECT_THAT(freshPass->setPipeline(pipeline_), IsOk());
  EXPECT_THAT(freshPass->setBindGroup(0, bindGroup_), IsOk());
  EXPECT_THAT(freshPass->dispatchWorkgroups(kMaxComputeWorkgroupsPerDimension + 1),
              IsGpuErrorWithMessage(GpuErrorType::LimitExceeded,
                                    HasSubstr("kMaxComputeWorkgroupsPerDimension")));
}

TEST_F(ComputePassTests, PassKindsAreMutuallyExclusive) {
  const Texture target = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView targetView =
      GetResultOrFail(device_.createTextureView(target, TextureViewDescriptor{"targetView"}));

  beginComputePass();
  EXPECT_THAT(
      encoder_->beginRenderPass(RenderPassDescriptor{
          "render", {RenderPassColorAttachment{targetView, LoadOp::Clear, StoreOp::Store, {}}}}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("already active")));
}

TEST_F(ComputePassTests, RenderPassBlocksBeginningAComputePass) {
  // The mirror of PassKindsAreMutuallyExclusive: exclusivity has to hold in both directions, or
  // one ordering of the same frame would silently open two passes at once.
  const Texture target = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView targetView =
      GetResultOrFail(device_.createTextureView(target, TextureViewDescriptor{"targetView"}));

  GetResultOrFail(encoder_->beginRenderPass(RenderPassDescriptor{
      "render", {RenderPassColorAttachment{targetView, LoadOp::Clear, StoreOp::Store, {}}}}));
  EXPECT_THAT(encoder_->beginComputePass(ComputePassDescriptor{"compute"}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("already active")));
}

TEST_F(ComputePassTests, ComputePassOpsAfterEndFail) {
  ComputePassEncoder* pass = beginComputePass();
  EXPECT_THAT(pass->end(), IsOk());
  EXPECT_THAT(
      pass->setPipeline(pipeline_),
      IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("no compute pass is active")));
}

TEST_F(ComputePassTests, CopyInsideAComputePassFails) {
  const Buffer readback = GetResultOrFail(device_.createBuffer(
      BufferDescriptor{"readback", 4096, BufferUsage::CopyDst | BufferUsage::MapRead}));
  beginComputePass();
  EXPECT_THAT(
      encoder_->copyTextureToBuffer(TexelCopyTextureInfo{output_}, readback,
                                    TexelCopyBufferLayout{0, 256, 16}, Extent2d{16, 16}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("not allowed inside a pass")));
}

TEST_F(ComputePassTests, FinishWithAnOpenComputePassFails) {
  beginComputePass();
  EXPECT_THAT(encoder_->finish(), IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                                        HasSubstr("a pass is still active")));
}

TEST_F(ComputePassTests, SubmitRejectsAComputePipelineDestroyedAfterRecording) {
  ComputePassEncoder* pass = beginComputePass();
  EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
  EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(1), IsOk());
  EXPECT_THAT(pass->end(), IsOk());
  auto finished = encoder_->finish();
  ASSERT_THAT(finished, HasResult());

  EXPECT_THAT(device_.destroyComputePipeline(std::move(pipeline_)), IsOk());
  EXPECT_THAT(device_.submit(std::move(finished).result()),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                                    HasSubstr("recorded compute setPipeline references destroyed "
                                              "computePipeline")));
}

TEST_F(ComputePassTests, CreateComputePipelineRejectsInvalidDescriptors) {
  ComputePipelineDescriptor emptyEntryPoint = pipelineDescriptor();
  emptyEntryPoint.compute.entryPoint = "";
  EXPECT_THAT(device_.createComputePipeline(emptyEntryPoint),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("compute.entryPoint is empty")));

  ComputePipelineDescriptor zeroSize = pipelineDescriptor();
  zeroSize.workgroupSize = WorkgroupSize{8, 0, 1};
  EXPECT_THAT(device_.createComputePipeline(zeroSize),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("8x0x1 has a zero dimension")));

  ComputePipelineDescriptor tooManyInvocations = pipelineDescriptor();
  tooManyInvocations.workgroupSize = WorkgroupSize{32, 32, 1};
  EXPECT_THAT(
      device_.createComputePipeline(tooManyInvocations),
      IsGpuErrorWithMessage(GpuErrorType::LimitExceeded, HasSubstr("declares 1024 invocations")));

  ComputePipelineDescriptor tooDeep = pipelineDescriptor();
  tooDeep.workgroupSize = WorkgroupSize{1, 1, kMaxComputeWorkgroupSizeZ + 1};
  EXPECT_THAT(device_.createComputePipeline(tooDeep),
              IsGpuErrorWithMessage(GpuErrorType::LimitExceeded,
                                    HasSubstr("exceeds the per-dimension caps")));
}

TEST_F(ComputePassTests, ComputePipelineRejectsAWorkgroupSizeTheShaderDoesNotDeclare) {
  // The descriptor's workgroup size is what Metal dispatches with, because Metal takes the
  // threadgroup shape per dispatch rather than from the compiled pipeline state. Every other
  // backend uses the size compiled into the shader. A descriptor that disagrees therefore runs a
  // different invocation grid on one backend than on the others, which shows up as silently
  // incomplete or overlapping output rather than as an error, so it has to be rejected here.
  ComputePipelineDescriptor mismatched = pipelineDescriptor();
  mismatched.workgroupSize = WorkgroupSize{16, 16, 1};
  EXPECT_THAT(device_.createComputePipeline(mismatched),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("workgroupSize 16x16x1 disagrees with the 8x8x1 "
                                              "that shader module \"colorMatrix\" declares for "
                                              "entry point \"csMain\"")));
}

TEST_F(ComputePassTests, ComputePipelineRejectsAnEntryPointTheModuleDoesNotDeclare) {
  ComputePipelineDescriptor unknownEntryPoint = pipelineDescriptor();
  unknownEntryPoint.compute.entryPoint = "csMissing";
  EXPECT_THAT(device_.createComputePipeline(unknownEntryPoint),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("does not declare a compute entry point named "
                                              "\"csMissing\"")));
}

TEST_F(ComputePassTests, ComputePipelineRejectsAModuleThatDeclaresNoComputeEntryPoints) {
  // A module that never says what it contains cannot be checked, so it cannot back a compute
  // pipeline; otherwise the guard above would be opt-in and silently absent wherever a caller
  // forgot to fill the list in.
  ShaderModuleDescriptor undeclared = shaderModuleDescriptor();
  undeclared.label = "undeclared";
  undeclared.computeEntryPoints.clear();
  const ShaderModule undeclaredModule = GetResultOrFail(device_.createShaderModule(undeclared));

  ComputePipelineDescriptor descriptor = pipelineDescriptor();
  descriptor.compute.module = undeclaredModule;
  EXPECT_THAT(device_.createComputePipeline(descriptor),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("declares no compute entry points")));
}

TEST_F(ComputePassTests, StorageTextureBindingRequiresTheStorageBindingUsage) {
  const Texture sampledOnly = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "sampledOnly", Extent2d{16, 16}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
  const TextureView sampledOnlyView = GetResultOrFail(
      device_.createTextureView(sampledOnly, TextureViewDescriptor{"sampledOnlyView"}));

  EXPECT_THAT(device_.createBindGroup(
                  BindGroupDescriptor{"bad", bindGroupLayout_, bindGroupEntries(sampledOnlyView)}),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch,
                                    HasSubstr("lacks the StorageBinding usage")));
}

TEST_F(ComputePassTests, StorageTextureBindingRequiresTheLayoutFormat) {
  const Texture wrongFormat = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "wrongFormat", Extent2d{16, 16}, TextureFormat::BGRA8Unorm, TextureUsage::StorageBinding}));
  const TextureView wrongFormatView = GetResultOrFail(
      device_.createTextureView(wrongFormat, TextureViewDescriptor{"wrongFormatView"}));

  EXPECT_THAT(device_.createBindGroup(
                  BindGroupDescriptor{"bad", bindGroupLayout_, bindGroupEntries(wrongFormatView)}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("does not match the layout's storageTextureFormat "
                                              "(BGRA8Unorm vs RGBA8Unorm)")));
}

TEST_F(ComputePassTests, BindGroupLayoutRejectsAnUnknownStorageTextureFormat) {
  std::vector<BindGroupLayoutEntry> entries = layoutEntries();
  entries[1].storageTextureFormat = static_cast<TextureFormat>(0x7F);
  EXPECT_THAT(device_.createBindGroupLayout(BindGroupLayoutDescriptor{"bad", std::move(entries)}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("BindGroupLayoutEntry.storageTextureFormat has "
                                              "unknown enum value")));
}

TEST_F(ComputePassTests, BindGroupRejectsOneTextureBoundBothSampledAndStorageWrite) {
  // A texture created with both usages can legally be named by a sampled binding and a
  // storage-write binding of the same group, and every per-entry check passes. The two bindings
  // then declare different layouts for one image, so whatever the backend transitions it to is
  // wrong for the other binding at dispatch. Nothing in the pipelines this runtime serves aliases
  // a texture both ways inside one pass, so the aliasing is rejected rather than modeled.
  const Texture aliased = GetResultOrFail(device_.createTexture(
      TextureDescriptor{"aliased", Extent2d{16, 16}, TextureFormat::RGBA8Unorm,
                        TextureUsage::Sampled | TextureUsage::StorageBinding}));
  const TextureView sampledView = GetResultOrFail(
      device_.createTextureView(aliased, TextureViewDescriptor{"aliasedSampledView"}));
  const TextureView storageView = GetResultOrFail(
      device_.createTextureView(aliased, TextureViewDescriptor{"aliasedStorageView"}));

  std::vector<BindGroupEntry> entries = bindGroupEntries(storageView);
  entries[0] = BindGroupEntry{0, TextureViewBinding{sampledView}};
  EXPECT_THAT(
      device_.createBindGroup(
          BindGroupDescriptor{"aliasing", bindGroupLayout_, std::move(entries)}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                            HasSubstr("texture \"aliased\" is bound as a sampled texture at "
                                      "binding 0 and as a storage texture at binding 1")));
}

TEST_F(ComputePassTests, BindGroupAcceptsDistinctSampledAndStorageTextures) {
  // The guard must not reject the ordinary shape: two different textures, one sampled and one
  // written, which is what every filter pass binds.
  EXPECT_THAT(device_.createBindGroup(
                  BindGroupDescriptor{"distinct", bindGroupLayout_, bindGroupEntries(outputView_)}),
              HasResult());
}

TEST_F(ComputePassTests, StorageTextureBindingRejectsANonTextureResource) {
  std::vector<BindGroupEntry> entries = bindGroupEntries(outputView_);
  entries[1] = BindGroupEntry{1, BufferBinding{params_, 0, 64}};
  EXPECT_THAT(
      device_.createBindGroup(BindGroupDescriptor{"bad", bindGroupLayout_, std::move(entries)}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                            HasSubstr("must bind a texture view to match the layout type "
                                      "WriteOnlyStorageTexture2d")));
}

TEST_F(ComputePassTests, RenderAndComputePassesInterleaveInOneCommandBuffer) {
  // The filter engine records compute passes into the same encoder as the renderer's draws, so
  // one command buffer must be able to carry both, in recording order.
  const Texture target = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView targetView =
      GetResultOrFail(device_.createTextureView(target, TextureViewDescriptor{"targetView"}));

  ComputePassEncoder* computePass = beginComputePass();
  EXPECT_THAT(computePass->setPipeline(pipeline_), IsOk());
  EXPECT_THAT(computePass->setBindGroup(0, bindGroup_), IsOk());
  EXPECT_THAT(computePass->dispatchWorkgroups(2, 2, 1), IsOk());
  EXPECT_THAT(computePass->end(), IsOk());

  RenderPassEncoder* renderPass = GetResultOrFail(encoder_->beginRenderPass(RenderPassDescriptor{
      "render", {RenderPassColorAttachment{targetView, LoadOp::Clear, StoreOp::Store, {}}}}));
  ASSERT_NE(renderPass, nullptr);
  EXPECT_THAT(renderPass->end(), IsOk());

  auto finished = encoder_->finish();
  ASSERT_THAT(finished, HasResult());
  EXPECT_THAT(device_.submit(std::move(finished).result()), HasResult());

  const std::string recording = device_.serialize();
  const size_t computeEnd = recording.find("  endComputePass");
  const size_t renderBegin = recording.find("  beginRenderPass");
  ASSERT_NE(computeEnd, std::string::npos);
  ASSERT_NE(renderBegin, std::string::npos);
  EXPECT_LT(computeEnd, renderBegin) << "the submission must preserve recording order";
}

}  // namespace
}  // namespace donner::gpu
