/// @file
/// Allocation budget for the encoder's per-draw revalidation.
///
/// This lives in its own binary on purpose. AllocationTracker.h defines global replacement
/// `operator new` and `operator delete`, which take precedence over the sanitizers' own
/// interceptors, so linking it into a general test binary would quietly weaken the ASan and UBSan
/// lanes for everything else in that binary. Keeping it here bounds the damage to one target that
/// those lanes skip, and keeps the header's non-inline operators to a single translation unit.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/benchmarks/AllocationTracker.h"

namespace donner::gpu {
namespace {

/// One sampled texture reached through one bind group: the smallest shape that exercises the
/// per-draw group revalidation and the sampled/storage role check together.
class EncoderAllocationTests : public testing::Test {
protected:
  void SetUp() override {
    texture_ = GetResultOrFail(device_.createTexture(
        TextureDescriptor{"shared", {16, 16}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
    view_ = GetResultOrFail(device_.createTextureView(texture_, {"sharedView"}));
    sampledLayout_ = GetResultOrFail(device_.createBindGroupLayout(
        {"sampled", {{0, ShaderStage::Compute, BindingType::SampledTexture2dFloat}}}));
    sampled_ = GetResultOrFail(device_.createBindGroup(
        {"textureGroup", sampledLayout_, {{0, TextureViewBinding{view_}}}}));
    shader_ = GetResultOrFail(device_.createShaderModule(
        ShaderModuleDescriptor{"noop",
                               "@compute @workgroup_size(8, 8, 1) fn csMain() {}",
                               ShaderSourceKind::Wgsl,
                               {},
                               {ComputeEntryPointInfo{"csMain", WorkgroupSize{8, 8, 1}}}}));
  }

  ComputePipeline makePipeline(std::vector<BindGroupLayoutRef> groups) {
    const PipelineLayout layout =
        GetResultOrFail(device_.createPipelineLayout({"activeGroups", std::move(groups)}));
    return GetResultOrFail(
        device_.createComputePipeline({"activeGroups", layout, {shader_, "csMain"}, {8, 8, 1}}));
  }

  RecordingDevice device_;
  Texture texture_;
  TextureView view_;
  BindGroupLayout sampledLayout_;
  BindGroup sampled_;
  ShaderModule shader_;
  std::unique_ptr<CommandEncoder> encoder_;
};

TEST_F(EncoderAllocationTests, RevalidationDoesNotAllocatePerDispatch) {
  // The role and lifetime checks run on every dispatch, on the path the Geode renderer records
  // through, so their cost must not grow with the dispatch count. Recording a command is itself an
  // amortized vector growth, so measure a pipeline with no bind groups as the baseline and require
  // that adding a validated group costs nothing beyond it. Both encoders enter the measurement
  // window having recorded the same number of commands, so the growth ahead of them matches.
  constexpr uint32_t kDispatches = 512;

  const auto measure = [&](bool withBoundGroup) {
    encoder_ = GetResultOrFail(device_.createCommandEncoder());
    const ComputePipeline pipeline =
        withBoundGroup ? makePipeline({sampledLayout_}) : makePipeline({});
    ComputePassEncoder* pass =
        GetResultOrFail(encoder_->beginComputePass(ComputePassDescriptor{"pass"}));
    EXPECT_THAT(pass->setPipeline(pipeline), IsOk());
    if (withBoundGroup) {
      EXPECT_THAT(pass->setBindGroup(0, sampled_), IsOk());
    } else {
      // Stands in for the setBindGroup command, so both encoders reach the window with the same
      // recorded-command count.
      EXPECT_THAT(pass->dispatchWorkgroups(1), IsOk());
    }
    EXPECT_THAT(pass->dispatchWorkgroups(1), IsOk());

    benchmarks::allocations::Scope allocations;
    for (uint32_t dispatch = 0; dispatch < kDispatches; ++dispatch) {
      EXPECT_THAT(pass->dispatchWorkgroups(1), IsOk());
    }
    return allocations.stop().allocationCalls;
  };

  // Not an equality assertion: the baseline window runs first, so a one-time lazy allocation in a
  // shared path would land there and make the validated count legitimately smaller. Two
  // allocations per dispatch, the regression this guards, is 1024 over this loop and cannot hide
  // under it. One group with one sampled texture stays inside the collectors' inline capacity, so
  // this does not cover a spill.
  const uint64_t baseline = measure(false);
  const uint64_t validated = measure(true);
  EXPECT_THAT(validated, testing::Le(baseline))
      << validated << " allocations with a bound group versus " << baseline
      << " recording the same " << kDispatches << " dispatches without one";
}

/// The same shape for the render path: one sampled texture reached through one bind group, drawn
/// with indexed draws so the index-range check and the vertex-slot check run alongside the group
/// revalidation.
class IndexedDrawAllocationTests : public testing::Test {
protected:
  void SetUp() override {
    texture_ = GetResultOrFail(device_.createTexture(
        TextureDescriptor{"shared", {16, 16}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
    view_ = GetResultOrFail(device_.createTextureView(texture_, {"sharedView"}));
    target_ = GetResultOrFail(device_.createTexture(TextureDescriptor{
        "target", {4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
    targetView_ = GetResultOrFail(device_.createTextureView(target_, {"targetView"}));
    sampledLayout_ = GetResultOrFail(device_.createBindGroupLayout(
        {"sampled", {{0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat}}}));
    sampled_ = GetResultOrFail(device_.createBindGroup(
        {"textureGroup", sampledLayout_, {{0, TextureViewBinding{view_}}}}));
    vertices_ = GetResultOrFail(
        device_.createBuffer(BufferDescriptor{"vertices", 48, BufferUsage::Vertex}));
    indices_ =
        GetResultOrFail(device_.createBuffer(BufferDescriptor{"indices", 24, BufferUsage::Index}));
    shader_ = GetResultOrFail(device_.createShaderModule(ShaderModuleDescriptor{
        "solidFill", "@vertex fn vsMain() {}\n@fragment fn fsMain() {}", ShaderSourceKind::Wgsl}));
  }

  RenderPipeline makePipeline(std::vector<BindGroupLayoutRef> groups) {
    const PipelineLayout layout =
        GetResultOrFail(device_.createPipelineLayout({"activeGroups", std::move(groups)}));
    return GetResultOrFail(device_.createRenderPipeline(RenderPipelineDescriptor{
        "activeGroups", layout,
        VertexState{
            shader_,
            "vsMain",
            {VertexBufferLayout{
                8, VertexStepMode::Vertex, {VertexAttribute{VertexFormat::Float32x2, 0, 0}}}}},
        FragmentState{shader_, "fsMain", {ColorTargetState{TextureFormat::RGBA8Unorm}}}}));
  }

  RecordingDevice device_;
  Texture texture_;
  TextureView view_;
  Texture target_;
  TextureView targetView_;
  BindGroupLayout sampledLayout_;
  BindGroup sampled_;
  Buffer vertices_;
  Buffer indices_;
  ShaderModule shader_;
  std::unique_ptr<CommandEncoder> encoder_;
};

TEST_F(IndexedDrawAllocationTests, RevalidationDoesNotAllocatePerIndexedDraw) {
  // Same measurement as the dispatch case: the encoder with a validated group must not allocate
  // more than the one without over the same number of recorded indexed draws.
  constexpr uint32_t kDraws = 512;

  const auto measure = [&](bool withBoundGroup) {
    encoder_ = GetResultOrFail(device_.createCommandEncoder());
    const RenderPipeline pipeline =
        withBoundGroup ? makePipeline({sampledLayout_}) : makePipeline({});
    RenderPassEncoder* pass = GetResultOrFail(encoder_->beginRenderPass(RenderPassDescriptor{
        "pass", {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store, {}}}}));
    EXPECT_THAT(pass->setPipeline(pipeline), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(0, vertices_), IsOk());
    EXPECT_THAT(pass->setIndexBuffer(indices_, IndexFormat::Uint16), IsOk());
    if (withBoundGroup) {
      EXPECT_THAT(pass->setBindGroup(0, sampled_), IsOk());
    } else {
      // Stands in for the setBindGroup command, so both encoders reach the window with the same
      // recorded-command count.
      EXPECT_THAT(pass->drawIndexed(12), IsOk());
    }
    EXPECT_THAT(pass->drawIndexed(12), IsOk());

    benchmarks::allocations::Scope allocations;
    for (uint32_t draw = 0; draw < kDraws; ++draw) {
      EXPECT_THAT(pass->drawIndexed(12), IsOk());
    }
    return allocations.stop().allocationCalls;
  };

  const uint64_t baseline = measure(false);
  const uint64_t validated = measure(true);
  EXPECT_THAT(validated, testing::Le(baseline))
      << validated << " allocations with a bound group versus " << baseline
      << " recording the same " << kDraws << " indexed draws without one";
}

}  // namespace
}  // namespace donner::gpu
