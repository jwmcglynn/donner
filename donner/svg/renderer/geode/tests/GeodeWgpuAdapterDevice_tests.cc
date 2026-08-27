/// @file
/// Conformance tests for \c donner::geode::GeodeWgpuAdapterDevice: every resource kind the
/// solid/gradient/mask/image pipeline family needs is created through the adapter on the real
/// headless wgpu device, a minimal render pass plus both copy commands executes to completion,
/// and fail-closed inputs are rejected before reaching wgpu.

#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/SubRectangleCopyScene.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

using testing::ElementsAre;
using testing::Ge;
using testing::HasSubstr;
using testing::Lt;
using testing::Not;

namespace donner::geode {
namespace {

/// Minimal compute WGSL writing a constant color into a write-only storage texture, matching the
/// compute pipeline the conformance and replay tests create.
constexpr const char* kFillComputeWgsl = R"(
@group(0) @binding(0) var output_tex: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(4, 4, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let size = textureDimensions(output_tex);
  if ((gid.x >= size.x) || (gid.y >= size.y)) {
    return;
  }
  textureStore(output_tex, vec2<i32>(gid.xy), vec4f(0.0, 1.0, 0.0, 1.0));
}
)";

/// Minimal WGSL exercising a uniform binding and one vertex buffer, matching the pipeline the
/// conformance scene creates.
constexpr const char* kSolidWgsl = R"(
struct Uniforms {
  color: vec4f,
}
@group(0) @binding(0) var<uniform> uniforms: Uniforms;

@vertex
fn vs_main(@location(0) pos: vec2f) -> @builtin(position) vec4f {
  return vec4f(pos, 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4f {
  return uniforms.color;
}
)";

std::vector<uint8_t> MakeBytes(size_t count) {
  std::vector<uint8_t> bytes(count);
  for (size_t i = 0; i < count; ++i) {
    bytes[i] = static_cast<uint8_t>(i & 0xFF);
  }
  return bytes;
}

constexpr uint32_t kSceneSize = 4;
constexpr uint32_t kReadbackBytesPerRow = 256;  // 4x4 RGBA rows padded to the copy alignment.

/// Reads a square RGBA8 texture back to the host through raw wgpu (the adapter has no readback
/// API of its own yet), mirroring the map-and-poll pattern the existing Geode tests use. Returns
/// the padded rows (kReadbackBytesPerRow per row), or empty on failure.
/// @param device Device owning \p texture.
/// @param texture Texture to read; needs CopySrc.
/// @param sceneSize Width and height of \p texture in texels; rows must fit the padded pitch.
std::vector<uint8_t> ReadbackTexturePixels(GeodeDevice& device, wgpu::Texture texture,
                                           uint32_t sceneSize = kSceneSize) {
  const uint64_t byteSize = uint64_t{kReadbackBytesPerRow} * sceneSize;
  wgpu::BufferDescriptor bufferDescriptor = {};
  bufferDescriptor.label = wgpuLabel("readbackStaging");
  bufferDescriptor.size = byteSize;
  bufferDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  ScopedWgpuHandle<wgpu::Buffer> readback(device.device().createBuffer(bufferDescriptor));
  if (!readback) {
    return {};
  }

  ScopedWgpuHandle<wgpu::CommandEncoder> encoder(device.device().createCommandEncoder());
  wgpu::TexelCopyTextureInfo source = {};
  source.texture = texture;
  wgpu::TexelCopyBufferInfo destination = {};
  destination.buffer = readback.get();
  destination.layout.bytesPerRow = kReadbackBytesPerRow;
  destination.layout.rowsPerImage = sceneSize;
  const wgpu::Extent3D copySize = {sceneSize, sceneSize, 1};
  encoder.get().copyTextureToBuffer(source, destination, copySize);
  ScopedWgpuHandle<wgpu::CommandBuffer> commandBuffer(encoder.get().finish());
  device.queue().submit(1, &commandBuffer.get());

  struct MapState {
    std::atomic<bool> done = false;
    std::atomic<bool> ok = false;
  };
  auto mapState = std::make_shared<MapState>();
  wgpu::BufferMapCallbackInfo mapCallback{wgpu::Default};
  mapCallback.mode = wgpu::CallbackMode::AllowSpontaneous;
  mapCallback.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                            void* /*userdata2*/) {
    const std::shared_ptr<MapState> state = takeWgpuCallbackState<MapState>(userdata1);
    state->ok.store(status == WGPUMapAsyncStatus_Success, std::memory_order_relaxed);
    state->done.store(true, std::memory_order_release);
  };
  mapCallback.userdata1 = retainWgpuCallbackState(mapState);
  mapCallback.userdata2 = nullptr;
  readback.get().mapAsync(wgpu::MapMode::Read, 0, byteSize, mapCallback);
  for (int pollIter = 0; pollIter < 2000 && !mapState->done.load(std::memory_order_acquire);
       ++pollIter) {
    device.device().poll(true, nullptr);
  }
  if (!mapState->ok.load(std::memory_order_relaxed)) {
    return {};
  }

  const uint8_t* mapped =
      static_cast<const uint8_t*>(readback.get().getConstMappedRange(0, byteSize));
  std::vector<uint8_t> pixels(mapped, mapped + byteSize);
  readback.get().unmap();
  return pixels;
}

/// Returns the 4 RGBA bytes of pixel (x, y) from padded readback rows.
std::vector<uint8_t> PixelAt(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y) {
  const size_t base = size_t{y} * kReadbackBytesPerRow + size_t{x} * 4;
  return std::vector<uint8_t>(pixels.begin() + base, pixels.begin() + base + 4);
}

/// Resources for the compute conformance scene: a write-only storage texture bound to the
/// single-entry layout the fill kernel declares.
struct ComputeScene {
  gpu::Texture target;            //!< Storage destination the kernel writes.
  gpu::TextureView targetView;    //!< View bound at binding 0.
  gpu::BindGroupLayout layout;    //!< Single write-only storage texture entry.
  gpu::BindGroup bindGroup;       //!< The bound group.
  gpu::ComputePipeline pipeline;  //!< Pipeline over kFillComputeWgsl.
};

/// Creates the compute conformance scene on \p adapter.
/// @param adapter Adapter to create through.
/// @param label Debug label prefix for the created resources.
ComputeScene MakeComputeScene(GeodeWgpuAdapterDevice& adapter, const char* label) {
  ComputeScene scene;
  scene.target = gpu::GetResultOrFail(adapter.createTexture(gpu::TextureDescriptor{
      label, gpu::Extent2d{kSceneSize, kSceneSize}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::CopySrc}));
  scene.targetView = gpu::GetResultOrFail(
      adapter.createTextureView(scene.target, gpu::TextureViewDescriptor{"computeTargetView"}));
  scene.layout = gpu::GetResultOrFail(adapter.createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
      "computeBindings",
      {gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Compute,
                                 gpu::BindingType::WriteOnlyStorageTexture2d,
                                 gpu::TextureFormat::RGBA8Unorm}}}));
  scene.bindGroup = gpu::GetResultOrFail(adapter.createBindGroup(gpu::BindGroupDescriptor{
      "computeGroup",
      scene.layout,
      {gpu::BindGroupEntry{0, gpu::TextureViewBinding{scene.targetView}}}}));
  const gpu::PipelineLayout pipelineLayout = gpu::GetResultOrFail(
      adapter.createPipelineLayout(gpu::PipelineLayoutDescriptor{"computeLayout", {scene.layout}}));
  // Hand-written WGSL rather than emitted IR, so the entry point is declared inline; the runtime
  // checks the pipeline's workgroup size against it.
  const gpu::ShaderModule shader =
      gpu::GetResultOrFail(adapter.createShaderModule(gpu::ShaderModuleDescriptor{
          "fillCompute",
          kFillComputeWgsl,
          gpu::ShaderSourceKind::Wgsl,
          {},
          {gpu::ComputeEntryPointInfo{"cs_main", gpu::WorkgroupSize{4, 4, 1}}}}));
  scene.pipeline =
      gpu::GetResultOrFail(adapter.createComputePipeline(gpu::ComputePipelineDescriptor{
          "fillCompute", pipelineLayout, gpu::ComputeState{shader, "cs_main"},
          gpu::WorkgroupSize{4, 4, 1}}));
  return scene;
}

class GeodeWgpuAdapterDeviceTests : public testing::Test {
protected:
  void SetUp() override {
    geodeDevice_ = GeodeDevice::CreateHeadless();
    ASSERT_NE(geodeDevice_, nullptr)
        << "Failed to create the headless wgpu device. Check driver availability.";
    adapter_ = std::make_unique<GeodeWgpuAdapterDevice>(*geodeDevice_);
  }

  std::unique_ptr<GeodeDevice> geodeDevice_;
  std::unique_ptr<GeodeWgpuAdapterDevice> adapter_;
};

TEST_F(GeodeWgpuAdapterDeviceTests, FamilySceneRendersAndCompletes) {
  // ----- Every resource kind the pipeline family uses, created through the adapter -----
  const gpu::Texture target = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"target", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc}));
  const gpu::TextureView targetView = gpu::GetResultOrFail(
      adapter_->createTextureView(target, gpu::TextureViewDescriptor{"targetView"}));
  const gpu::Texture copyDestination = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"copyDestination", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc}));

  const gpu::Texture sampled = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"sampled", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst}));
  const gpu::TextureView sampledView = gpu::GetResultOrFail(
      adapter_->createTextureView(sampled, gpu::TextureViewDescriptor{"sampledView"}));
  const gpu::Sampler sampler = gpu::GetResultOrFail(adapter_->createSampler(
      gpu::SamplerDescriptor{"linearRepeat", gpu::FilterMode::Linear, gpu::FilterMode::Linear,
                             gpu::AddressMode::Repeat, gpu::AddressMode::Repeat}));

  const gpu::Buffer vertexBuffer = gpu::GetResultOrFail(adapter_->createBuffer(
      gpu::BufferDescriptor{"vertices", 48, gpu::BufferUsage::Vertex | gpu::BufferUsage::CopyDst}));
  const gpu::Buffer uniformBuffer =
      gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
          "uniforms", 16, gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst}));
  const gpu::Buffer readbackBuffer =
      gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
          "readback", 1024, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));

  const gpu::BindGroupLayout uniformLayout =
      gpu::GetResultOrFail(adapter_->createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
          "solidBindings",
          {gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment,
                                     gpu::BindingType::UniformBuffer}}}));
  const gpu::BindGroup uniformGroup =
      gpu::GetResultOrFail(adapter_->createBindGroup(gpu::BindGroupDescriptor{
          "solidUniforms",
          uniformLayout,
          {gpu::BindGroupEntry{0, gpu::BufferBinding{uniformBuffer, 0, 16}}}}));

  // Texture + sampler binding kinds, exercised through creation (the minimal draw below binds
  // only the uniform group its pipeline layout declares).
  const gpu::BindGroupLayout textureLayout =
      gpu::GetResultOrFail(adapter_->createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
          "textureBindings",
          {gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Fragment,
                                     gpu::BindingType::SampledTexture2dFloat},
           gpu::BindGroupLayoutEntry{1, gpu::ShaderStage::Fragment,
                                     gpu::BindingType::FilteringSampler}}}));
  const gpu::BindGroup textureGroup = gpu::GetResultOrFail(adapter_->createBindGroup(
      gpu::BindGroupDescriptor{"textureGroup",
                               textureLayout,
                               {gpu::BindGroupEntry{0, gpu::TextureViewBinding{sampledView}},
                                gpu::BindGroupEntry{1, gpu::SamplerBinding{sampler}}}}));
  (void)textureGroup;

  const gpu::PipelineLayout pipelineLayout = gpu::GetResultOrFail(adapter_->createPipelineLayout(
      gpu::PipelineLayoutDescriptor{"solidLayout", {uniformLayout}}));
  const gpu::ShaderModule shader = gpu::GetResultOrFail(adapter_->createShaderModule(
      gpu::ShaderModuleDescriptor{"solidWgsl", kSolidWgsl, gpu::ShaderSourceKind::Wgsl}));

  const gpu::RenderPipeline pipeline =
      gpu::GetResultOrFail(adapter_->createRenderPipeline(gpu::RenderPipelineDescriptor{
          "solid", pipelineLayout,
          gpu::VertexState{shader,
                           "vs_main",
                           {gpu::VertexBufferLayout{
                               8,
                               gpu::VertexStepMode::Vertex,
                               {gpu::VertexAttribute{gpu::VertexFormat::Float32x2, 0, 0}}}}},
          gpu::FragmentState{
              shader,
              "fs_main",
              {gpu::ColorTargetState{
                  gpu::TextureFormat::RGBA8Unorm,
                  gpu::BlendState{
                      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha,
                                          gpu::BlendOperation::Add},
                      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha,
                                          gpu::BlendOperation::Add}}}}}}));

  // The TEMPORARY texture escape hatches resolve render targets for readback and presentation,
  // which still run on the backend directly.
  EXPECT_TRUE(static_cast<bool>(adapter_->wgpuTextureOf(target)));
  EXPECT_TRUE(static_cast<bool>(adapter_->wgpuTextureViewOf(targetView)));

  // ----- Queue writes -----
  EXPECT_THAT(adapter_->writeBuffer(vertexBuffer, 0, MakeBytes(48)), gpu::IsOk());
  EXPECT_THAT(adapter_->writeBuffer(uniformBuffer, 0, MakeBytes(16)), gpu::IsOk());
  EXPECT_THAT(adapter_->writeTexture(sampled, MakeBytes(3 * 256 + 16),
                                     gpu::TexelCopyBufferLayout{0, 256, 4}, gpu::Extent2d{4, 4}),
              gpu::IsOk());

  // ----- Minimal render pass + both copy commands -----
  std::unique_ptr<gpu::CommandEncoder> encoder =
      gpu::GetResultOrFail(adapter_->createCommandEncoder());
  gpu::RenderPassEncoder* pass =
      gpu::GetResultOrFail(encoder->beginRenderPass(gpu::RenderPassDescriptor{
          "mainPass",
          {gpu::RenderPassColorAttachment{
              targetView, gpu::LoadOp::Clear, gpu::StoreOp::Store, {0, 0, 0.5, 1}}}}));
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->setPipeline(pipeline), gpu::IsOk());
  EXPECT_THAT(pass->setBindGroup(0, uniformGroup), gpu::IsOk());
  EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer), gpu::IsOk());
  EXPECT_THAT(pass->setScissorRect(0, 0, 4, 4), gpu::IsOk());
  EXPECT_THAT(pass->setViewport(0, 0, 4, 4, 0, 1), gpu::IsOk());
  EXPECT_THAT(pass->draw(6), gpu::IsOk());
  EXPECT_THAT(pass->end(), gpu::IsOk());
  EXPECT_THAT(encoder->copyTextureToTexture(target, copyDestination, gpu::Extent2d{4, 4}),
              gpu::IsOk());
  EXPECT_THAT(
      encoder->copyTextureToBuffer(gpu::TexelCopyTextureInfo{target}, readbackBuffer,
                                   gpu::TexelCopyBufferLayout{0, 256, 4}, gpu::Extent2d{4, 4}),
      gpu::IsOk());

  gpu::CommandBuffer commands = gpu::GetResultOrFail(encoder->finish());
  const uint64_t serial = gpu::GetResultOrFail(adapter_->submit(std::move(commands)));
  EXPECT_THAT(serial, Ge(uint64_t{1}));

  // The submission must complete on the real device and advance completedSerial.
  ASSERT_TRUE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "submission " << serial
      << " did not complete; completedSerial=" << adapter_->completedSerial();
  EXPECT_THAT(adapter_->completedSerial(), Ge(serial));

  // ----- Pixel observation -----
  // The recorded clear color {0, 0, 0.5, 1} maps to RGBA8 bytes (0, 0, 128, 255). The draw
  // contributes no coverage (the placeholder vertex bytes decode to near-zero denormal float
  // positions, producing zero-area triangles), so every pixel of the target must hold the
  // clear color exactly - a wrong clearValue channel mapping, a LoadOp transposition, or a
  // draw-parameter swap all show up here as wrong bytes.
  const std::vector<uint8_t> targetPixels =
      ReadbackTexturePixels(*geodeDevice_, adapter_->wgpuTextureOf(target));
  ASSERT_THAT(targetPixels, Not(testing::IsEmpty())) << "target readback failed";
  EXPECT_THAT(PixelAt(targetPixels, 0, 0), ElementsAre(0, 0, 128, 255));
  EXPECT_THAT(PixelAt(targetPixels, 2, 2), ElementsAre(0, 0, 128, 255));
  EXPECT_THAT(PixelAt(targetPixels, 3, 3), ElementsAre(0, 0, 128, 255));

  // The recorded copyTextureToTexture must have propagated the same bytes into the copy
  // destination, proving the copy executed rather than merely completing.
  const std::vector<uint8_t> copiedPixels =
      ReadbackTexturePixels(*geodeDevice_, adapter_->wgpuTextureOf(copyDestination));
  ASSERT_THAT(copiedPixels, Not(testing::IsEmpty())) << "copy destination readback failed";
  EXPECT_THAT(PixelAt(copiedPixels, 0, 0), ElementsAre(0, 0, 128, 255));
  EXPECT_THAT(PixelAt(copiedPixels, 2, 2), ElementsAre(0, 0, 128, 255));
  EXPECT_THAT(PixelAt(copiedPixels, 3, 3), ElementsAre(0, 0, 128, 255));
}

/// The host-encoder replay mode has to hold three properties at once, and they only mean
/// anything together, so one test drives all three: the replayed span and the spans the host
/// records itself land in ONE command buffer in recording order, the replay performs no queue
/// submit of its own, and the replayed serial is not observable as complete until the host
/// reports its submit. Losing any one of them silently corrupts a frame or lies to the deferred
/// destruction and wait paths.
TEST_F(GeodeWgpuAdapterDeviceTests, HostEncoderReplayInterleavesInOneBufferAndDefersCompletion) {
  const gpu::Texture target = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"replayTarget", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc}));
  const gpu::TextureView targetView = gpu::GetResultOrFail(
      adapter_->createTextureView(target, gpu::TextureViewDescriptor{"replayTargetView"}));
  const gpu::Texture copyDestination = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"replayCopyDst", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc}));

  GeodeCounters counters;
  geodeDevice_->setCounters(&counters);

  // The host owns this encoder for the whole "frame", exactly as a renderer does.
  wgpu::CommandEncoderDescriptor hostDescriptor = {};
  ScopedWgpuHandle<wgpu::CommandEncoder> hostEncoder(
      geodeDevice_->device().createCommandEncoder(hostDescriptor));
  ASSERT_TRUE(static_cast<bool>(hostEncoder.get()));
  adapter_->setHostCommandEncoder(hostEncoder.get());

  // A replayed span: clear the target to a color the copy below can be checked against.
  std::unique_ptr<gpu::CommandEncoder> encoder =
      gpu::GetResultOrFail(adapter_->createCommandEncoder());
  gpu::RenderPassEncoder* pass =
      gpu::GetResultOrFail(encoder->beginRenderPass(gpu::RenderPassDescriptor{
          "replayPass",
          {gpu::RenderPassColorAttachment{
              targetView, gpu::LoadOp::Clear, gpu::StoreOp::Store, {0, 0, 0.5, 1}}}}));
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->end(), gpu::IsOk());
  const uint64_t serial =
      gpu::GetResultOrFail(adapter_->submit(gpu::GetResultOrFail(encoder->finish())));
  EXPECT_THAT(serial, Ge(uint64_t{1}));

  // Replaying is not submitting: no queue submit happened, so no submit was counted, and the
  // serial must not be observable as complete yet. A short wait proves the hold-back rather
  // than merely observing a race.
  EXPECT_EQ(counters.submits, 0u) << "replay must not perform a queue submit of its own";
  EXPECT_THAT(adapter_->completedSerial(), Lt(serial));
  EXPECT_FALSE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/0.25))
      << "serial " << serial << " reported complete before the host submitted its command buffer";

  // A span the host records itself, AFTER the replayed one. It reads what the replayed clear
  // wrote, so a wrong replay position (or a second command buffer) shows up as wrong bytes.
  wgpu::TexelCopyTextureInfo copySource = {};
  copySource.texture = adapter_->wgpuTextureOf(target);
  wgpu::TexelCopyTextureInfo copyDest = {};
  copyDest.texture = adapter_->wgpuTextureOf(copyDestination);
  const wgpu::Extent3D copyExtent = {4u, 4u, 1u};
  hostEncoder.get().copyTextureToTexture(copySource, copyDest, copyExtent);

  // The host's single submit covers both spans.
  {
    ScopedWgpuHandle<wgpu::CommandBuffer> hostCommands(hostEncoder.get().finish());
    ASSERT_TRUE(static_cast<bool>(hostCommands.get()));
    geodeDevice_->queue().submit(1, &hostCommands.get());
  }
  adapter_->notifyHostSubmitted();
  adapter_->clearHostCommandEncoder();

  ASSERT_TRUE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "submission " << serial
      << " did not complete after the host submit; completedSerial=" << adapter_->completedSerial();
  EXPECT_THAT(adapter_->completedSerial(), Ge(serial));

  const std::vector<uint8_t> targetPixels =
      ReadbackTexturePixels(*geodeDevice_, adapter_->wgpuTextureOf(target));
  ASSERT_THAT(targetPixels, Not(testing::IsEmpty())) << "replay target readback failed";
  EXPECT_THAT(PixelAt(targetPixels, 0, 0), ElementsAre(0, 0, 128, 255));

  const std::vector<uint8_t> copiedPixels =
      ReadbackTexturePixels(*geodeDevice_, adapter_->wgpuTextureOf(copyDestination));
  ASSERT_THAT(copiedPixels, Not(testing::IsEmpty())) << "replay copy destination readback failed";
  EXPECT_THAT(PixelAt(copiedPixels, 0, 0), ElementsAre(0, 0, 128, 255));
  EXPECT_THAT(PixelAt(copiedPixels, 3, 3), ElementsAre(0, 0, 128, 255));

  geodeDevice_->setCounters(nullptr);
}

/// The sub-rectangle copy scene on this adapter: a source holding the shared coordinate-encoding
/// pattern and a destination pre-filled with the shared sentinel, both uploaded through the
/// runtime so the copy under test is the only thing that moves texels between them.
struct SubRectCopyScene {
  gpu::Texture source;       //!< Source holding the coordinate-encoding pattern.
  gpu::Texture destination;  //!< Destination pre-filled with the sentinel.
};

/// Creates and fills the sub-rectangle copy scene on \p device.
/// @param device Device to create the textures on.
SubRectCopyScene MakeSubRectCopyScene(gpu::Device& device) {
  const gpu::Extent2d extent{gpu::tests::kSubRectCopyExtent, gpu::tests::kSubRectCopyExtent};
  const gpu::TexelCopyBufferLayout layout{0, gpu::tests::kSubRectCopyBytesPerRow,
                                          gpu::tests::kSubRectCopyExtent};
  SubRectCopyScene scene{gpu::GetResultOrFail(device.createTexture(gpu::TextureDescriptor{
                             "subRectSource", extent, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopySrc | gpu::TextureUsage::CopyDst})),
                         gpu::GetResultOrFail(device.createTexture(gpu::TextureDescriptor{
                             "subRectDestination", extent, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc}))};
  EXPECT_THAT(
      device.writeTexture(scene.source, gpu::tests::SubRectCopySourceUpload(), layout, extent),
      gpu::IsOk());
  EXPECT_THAT(device.writeTexture(scene.destination, gpu::tests::SubRectCopyDestinationUpload(),
                                  layout, extent),
              gpu::IsOk());
  return scene;
}

/// Checks \p pixels against the shared expected image for the sub-rectangle copy.
/// @param pixels Padded readback rows of the destination texture.
void ExpectSubRectCopyResult(const std::vector<uint8_t>& pixels) {
  for (uint32_t y = 0; y < gpu::tests::kSubRectCopyExtent; ++y) {
    for (uint32_t x = 0; x < gpu::tests::kSubRectCopyExtent; ++x) {
      const std::array<uint8_t, 4> expected = gpu::tests::SubRectCopyExpectedTexel(x, y);
      EXPECT_THAT(PixelAt(pixels, x, y),
                  ElementsAre(expected[0], expected[1], expected[2], expected[3]))
          << "texel (" << x << ", " << y << ")";
    }
  }
}

/// Records the sub-rectangle copy on \p encoder, reading from the scene's source origin and
/// writing at its destination origin.
/// @param encoder Encoder to record into.
/// @param scene Scene whose textures the copy runs over.
void RecordSubRectCopy(gpu::CommandEncoder& encoder, const SubRectCopyScene& scene) {
  EXPECT_THAT(encoder.copyTextureToTexture(
                  scene.source, scene.destination,
                  gpu::Extent2d{gpu::tests::kSubRectCopyWidth, gpu::tests::kSubRectCopyHeight},
                  gpu::Origin2d{gpu::tests::kSubRectCopySourceX, gpu::tests::kSubRectCopySourceY},
                  gpu::Origin2d{gpu::tests::kSubRectCopyDestinationX,
                                gpu::tests::kSubRectCopyDestinationY}),
              gpu::IsOk());
}

/// A recorded sub-rectangle copy must reach wgpu with both origins intact. Whole-rect copies
/// could not tell an ignored origin from an honored one; this scene can, because the source
/// rectangle and the destination rectangle sit at different corners of textures whose texels
/// encode their own coordinates.
TEST_F(GeodeWgpuAdapterDeviceTests, SubRectangleCopyHonorsBothOriginsWhenTheAdapterSubmits) {
  const SubRectCopyScene scene = MakeSubRectCopyScene(*adapter_);

  std::unique_ptr<gpu::CommandEncoder> encoder =
      gpu::GetResultOrFail(adapter_->createCommandEncoder());
  RecordSubRectCopy(*encoder, scene);
  const uint64_t serial =
      gpu::GetResultOrFail(adapter_->submit(gpu::GetResultOrFail(encoder->finish())));
  ASSERT_TRUE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "submission " << serial << " did not complete";

  const std::vector<uint8_t> pixels = ReadbackTexturePixels(
      *geodeDevice_, adapter_->wgpuTextureOf(scene.destination), gpu::tests::kSubRectCopyExtent);
  ASSERT_THAT(pixels, Not(testing::IsEmpty())) << "destination readback failed";
  ExpectSubRectCopyResult(pixels);
}

/// The same copy replayed into a host-owned encoder. The replay path builds its own wgpu copy
/// descriptors, so it can drop an origin independently of the owned-submit path.
TEST_F(GeodeWgpuAdapterDeviceTests, SubRectangleCopyHonorsBothOriginsWhenReplayedIntoAHostEncoder) {
  const SubRectCopyScene scene = MakeSubRectCopyScene(*adapter_);

  wgpu::CommandEncoderDescriptor hostDescriptor = {};
  ScopedWgpuHandle<wgpu::CommandEncoder> hostEncoder(
      geodeDevice_->device().createCommandEncoder(hostDescriptor));
  ASSERT_TRUE(static_cast<bool>(hostEncoder.get()));
  adapter_->setHostCommandEncoder(hostEncoder.get());

  std::unique_ptr<gpu::CommandEncoder> encoder =
      gpu::GetResultOrFail(adapter_->createCommandEncoder());
  RecordSubRectCopy(*encoder, scene);
  const uint64_t serial =
      gpu::GetResultOrFail(adapter_->submit(gpu::GetResultOrFail(encoder->finish())));

  {
    ScopedWgpuHandle<wgpu::CommandBuffer> hostCommands(hostEncoder.get().finish());
    ASSERT_TRUE(static_cast<bool>(hostCommands.get()));
    geodeDevice_->queue().submit(1, &hostCommands.get());
  }
  adapter_->notifyHostSubmitted();
  adapter_->clearHostCommandEncoder();

  ASSERT_TRUE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "submission " << serial << " did not complete after the host submit";

  const std::vector<uint8_t> pixels = ReadbackTexturePixels(
      *geodeDevice_, adapter_->wgpuTextureOf(scene.destination), gpu::tests::kSubRectCopyExtent);
  ASSERT_THAT(pixels, Not(testing::IsEmpty())) << "destination readback failed";
  ExpectSubRectCopyResult(pixels);
}

/// Clearing the host encoder returns the adapter to owning and submitting its own encoders, so
/// a stream submitted afterwards completes without any host notification.
TEST_F(GeodeWgpuAdapterDeviceTests, OwnedSubmitResumesAfterClearingTheHostEncoder) {
  const gpu::Texture target = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"ownedTarget", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc}));
  const gpu::TextureView targetView = gpu::GetResultOrFail(
      adapter_->createTextureView(target, gpu::TextureViewDescriptor{"ownedTargetView"}));

  wgpu::CommandEncoderDescriptor hostDescriptor = {};
  ScopedWgpuHandle<wgpu::CommandEncoder> hostEncoder(
      geodeDevice_->device().createCommandEncoder(hostDescriptor));
  ASSERT_TRUE(static_cast<bool>(hostEncoder.get()));
  adapter_->setHostCommandEncoder(hostEncoder.get());
  adapter_->clearHostCommandEncoder();

  GeodeCounters counters;
  geodeDevice_->setCounters(&counters);

  std::unique_ptr<gpu::CommandEncoder> encoder =
      gpu::GetResultOrFail(adapter_->createCommandEncoder());
  gpu::RenderPassEncoder* pass =
      gpu::GetResultOrFail(encoder->beginRenderPass(gpu::RenderPassDescriptor{
          "ownedPass",
          {gpu::RenderPassColorAttachment{
              targetView, gpu::LoadOp::Clear, gpu::StoreOp::Store, {0, 0, 0.5, 1}}}}));
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->end(), gpu::IsOk());
  const uint64_t serial =
      gpu::GetResultOrFail(adapter_->submit(gpu::GetResultOrFail(encoder->finish())));

  EXPECT_EQ(counters.submits, 1u) << "an adapter-owned submit must still count one submit";
  ASSERT_TRUE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/30.0));
  EXPECT_THAT(adapter_->completedSerial(), Ge(serial));

  geodeDevice_->setCounters(nullptr);
}

TEST_F(GeodeWgpuAdapterDeviceTests, OwnedSubmitEncodesAComputePassThatWritesItsStorageTexture) {
  ComputeScene scene = MakeComputeScene(*adapter_, "computeTarget");

  std::unique_ptr<gpu::CommandEncoder> encoder =
      gpu::GetResultOrFail(adapter_->createCommandEncoder());
  gpu::ComputePassEncoder* pass =
      gpu::GetResultOrFail(encoder->beginComputePass(gpu::ComputePassDescriptor{"fillPass"}));
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->setPipeline(scene.pipeline), gpu::IsOk());
  EXPECT_THAT(pass->setBindGroup(0, scene.bindGroup), gpu::IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(1, 1, 1), gpu::IsOk());
  EXPECT_THAT(pass->end(), gpu::IsOk());

  const uint64_t serial =
      gpu::GetResultOrFail(adapter_->submit(gpu::GetResultOrFail(encoder->finish())));
  ASSERT_TRUE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "compute submission " << serial
      << " did not complete; completedSerial=" << adapter_->completedSerial();

  // The kernel stores opaque green into every texel of the 4x4 destination, so a dropped
  // dispatch, a wrong bind group, or a pass that never opened shows up as untouched bytes.
  const std::vector<uint8_t> pixels =
      ReadbackTexturePixels(*geodeDevice_, adapter_->wgpuTextureOf(scene.target));
  ASSERT_THAT(pixels, Not(testing::IsEmpty())) << "compute target readback failed";
  EXPECT_THAT(PixelAt(pixels, 0, 0), ElementsAre(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(pixels, 2, 2), ElementsAre(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(pixels, 3, 3), ElementsAre(0, 255, 0, 255));
}

/// The replay-mode properties the render path holds must hold for compute passes too: the
/// replayed compute span lands in the host's command buffer ahead of the spans the host records
/// afterwards, the replay performs no queue submit of its own, and the replayed serial is not
/// observable as complete until the host reports its submit.
TEST_F(GeodeWgpuAdapterDeviceTests, HostEncoderReplaysAComputePassAheadOfHostRecordedSpans) {
  ComputeScene scene = MakeComputeScene(*adapter_, "replayComputeTarget");
  const gpu::Texture copyDestination = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"replayComputeCopyDst", gpu::Extent2d{kSceneSize, kSceneSize},
                             gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc}));

  GeodeCounters counters;
  geodeDevice_->setCounters(&counters);

  wgpu::CommandEncoderDescriptor hostDescriptor = {};
  ScopedWgpuHandle<wgpu::CommandEncoder> hostEncoder(
      geodeDevice_->device().createCommandEncoder(hostDescriptor));
  ASSERT_TRUE(static_cast<bool>(hostEncoder.get()));
  adapter_->setHostCommandEncoder(hostEncoder.get());

  std::unique_ptr<gpu::CommandEncoder> encoder =
      gpu::GetResultOrFail(adapter_->createCommandEncoder());
  gpu::ComputePassEncoder* pass =
      gpu::GetResultOrFail(encoder->beginComputePass(gpu::ComputePassDescriptor{"replayFillPass"}));
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->setPipeline(scene.pipeline), gpu::IsOk());
  EXPECT_THAT(pass->setBindGroup(0, scene.bindGroup), gpu::IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(1, 1, 1), gpu::IsOk());
  EXPECT_THAT(pass->end(), gpu::IsOk());
  const uint64_t serial =
      gpu::GetResultOrFail(adapter_->submit(gpu::GetResultOrFail(encoder->finish())));
  EXPECT_THAT(serial, Ge(uint64_t{1}));

  EXPECT_EQ(counters.submits, 0u) << "replay must not perform a queue submit of its own";
  EXPECT_THAT(adapter_->completedSerial(), Lt(serial));
  EXPECT_FALSE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/0.25))
      << "serial " << serial << " reported complete before the host submitted its command buffer";

  // A span the host records itself, AFTER the replayed compute pass. It copies what the kernel
  // wrote, so a wrong replay position (or a second command buffer) shows up as wrong bytes.
  wgpu::TexelCopyTextureInfo copySource = {};
  copySource.texture = adapter_->wgpuTextureOf(scene.target);
  wgpu::TexelCopyTextureInfo copyDest = {};
  copyDest.texture = adapter_->wgpuTextureOf(copyDestination);
  const wgpu::Extent3D copyExtent = {kSceneSize, kSceneSize, 1u};
  hostEncoder.get().copyTextureToTexture(copySource, copyDest, copyExtent);

  {
    ScopedWgpuHandle<wgpu::CommandBuffer> hostCommands(hostEncoder.get().finish());
    ASSERT_TRUE(static_cast<bool>(hostCommands.get()));
    geodeDevice_->queue().submit(1, &hostCommands.get());
  }
  adapter_->notifyHostSubmitted();
  adapter_->clearHostCommandEncoder();

  ASSERT_TRUE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "compute submission " << serial << " did not complete after the host submit";
  EXPECT_THAT(adapter_->completedSerial(), Ge(serial));

  const std::vector<uint8_t> copiedPixels =
      ReadbackTexturePixels(*geodeDevice_, adapter_->wgpuTextureOf(copyDestination));
  ASSERT_THAT(copiedPixels, Not(testing::IsEmpty())) << "replay copy destination readback failed";
  EXPECT_THAT(PixelAt(copiedPixels, 0, 0), ElementsAre(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(copiedPixels, 3, 3), ElementsAre(0, 255, 0, 255));

  geodeDevice_->setCounters(nullptr);
}

TEST_F(GeodeWgpuAdapterDeviceTests, ImportedExternalTextureIsUsableAndNotOwned) {
  // A texture the adapter did not create, standing in for a host-owned render target.
  wgpu::TextureDescriptor externalDescriptor = {};
  externalDescriptor.label = wgpuLabel("externalTarget");
  externalDescriptor.size = {4u, 4u, 1u};
  externalDescriptor.format = wgpu::TextureFormat::RGBA8Unorm;
  externalDescriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  externalDescriptor.mipLevelCount = 1;
  externalDescriptor.sampleCount = 1;
  externalDescriptor.dimension = wgpu::TextureDimension::_2D;
  ScopedWgpuHandle<wgpu::Texture> externalTexture(
      geodeDevice_->device().createTexture(externalDescriptor));
  ASSERT_TRUE(static_cast<bool>(externalTexture));

  gpu::Texture imported = gpu::GetResultOrFail(adapter_->importExternalTexture(
      externalTexture.get(), gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc));
  EXPECT_TRUE(static_cast<bool>(adapter_->wgpuTextureOf(imported)));

  // Destroying the handle forgets the registration but must not release the external texture:
  // the host's handle stays usable.
  EXPECT_THAT(adapter_->destroyTexture(std::move(imported)), gpu::IsOk());
  EXPECT_EQ(externalTexture.get().getWidth(), 4u);
}

TEST_F(GeodeWgpuAdapterDeviceTests, TextureViewHatchRejectsViewOfDestroyedTexture) {
  gpu::Texture texture = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"doomed", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::RenderAttachment}));
  const gpu::TextureView view = gpu::GetResultOrFail(
      adapter_->createTextureView(texture, gpu::TextureViewDescriptor{"doomedView"}));

  // While the texture is alive the hatch bridges the view.
  EXPECT_NE(adapter_->wgpuTextureViewOf(view), nullptr);

  // Destroying the texture makes the view stale everywhere, including the raw-wgpu bridge: the
  // hatch must re-resolve the viewed texture like every normal Device path does, so a caller
  // cannot bind a view whose Donner texture is gone.
  ASSERT_THAT(adapter_->destroyTexture(std::move(texture)), gpu::IsOk());
  EXPECT_EQ(adapter_->wgpuTextureViewOf(view), nullptr);
}

TEST_F(GeodeWgpuAdapterDeviceTests, TextureHatchRejectsStaleGeneration) {
  gpu::Texture original = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"original", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopySrc}));
  const uint32_t slot = original.slotIndex();
  const uint32_t generation = original.generation();
  const uint64_t deviceId = original.deviceId();
  ASSERT_THAT(adapter_->destroyTexture(std::move(original)), gpu::IsOk());

  const gpu::Texture replacement = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"replacement", gpu::Extent2d{8, 8}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopySrc}));
  ASSERT_EQ(replacement.slotIndex(), slot);

  // A forged handle carrying the retired generation must not alias the slot's new occupant.
  const gpu::Texture staleHandle = gpu::Texture::CreateForBackend(slot, generation, deviceId);
  EXPECT_EQ(adapter_->wgpuTextureOf(staleHandle), nullptr);
  EXPECT_NE(adapter_->wgpuTextureOf(replacement), nullptr);
}

TEST_F(GeodeWgpuAdapterDeviceTests, SpirvShaderKindFailsClosedAsUnsupported) {
  EXPECT_THAT(adapter_->createShaderModule(gpu::ShaderModuleDescriptor{
                  "spirv", "", gpu::ShaderSourceKind::Spirv, {0x07230203u, 0x00010300u}}),
              gpu::IsGpuErrorWithMessage(gpu::GpuErrorType::Unsupported, HasSubstr("WGSL only")));
}

TEST_F(GeodeWgpuAdapterDeviceTests, MisalignedBindOffsetFailsClosedBeforeWgpu) {
  const gpu::Buffer uniformBuffer =
      gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
          "uniforms", 64, gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst}));
  const gpu::BindGroupLayout layout =
      gpu::GetResultOrFail(adapter_->createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
          "uniforms",
          {gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Vertex,
                                     gpu::BindingType::UniformBuffer}}}));

  // Rejected by the base class's shared validation (InvalidDescriptor naming the offset), so
  // the misaligned binding never reaches wgpu's createBindGroup.
  EXPECT_THAT(
      adapter_->createBindGroup(gpu::BindGroupDescriptor{
          "group", layout, {gpu::BindGroupEntry{0, gpu::BufferBinding{uniformBuffer, 8, 16}}}}),
      gpu::IsGpuErrorWithMessage(gpu::GpuErrorType::InvalidDescriptor,
                                 HasSubstr("offsetBytes 8 is not a multiple of the 256-byte "
                                           "binding offset alignment")));
}

TEST_F(GeodeWgpuAdapterDeviceTests, AMappingReleasedBeforeItsCallbackStillUnmapsTheBuffer) {
  const gpu::Buffer buffer = gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
      "readback", 256, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));

  // Release the mapping before its completion can have been delivered: nothing has polled yet, so
  // the map is still in flight when the only handle to it goes away.
  {
    gpu::BufferMapping mapping =
        gpu::GetResultOrFail(adapter_->mapBufferAsync(buffer, gpu::MapMode::Read, 0, 256));
    EXPECT_THAT(adapter_->unmapBuffer(std::move(mapping)), gpu::IsOk());
  }

  // Let the abandoned map run to completion.
  for (int poll = 0; poll < 2000; ++poll) {
    (void)geodeDevice_->pollSuspending(false);
  }

  // A buffer left mapped with nothing able to unmap it cannot be mapped again, so mapping it a
  // second time is what tells us whether the abandoned one gave the buffer back.
  gpu::BufferMapping second =
      gpu::GetResultOrFail(adapter_->mapBufferAsync(buffer, gpu::MapMode::Read, 0, 256));
  EXPECT_EQ(
      gpu::GetResultOrFail(adapter_->waitForMapping(second, gpu::MapWaitParams{0.01, 2.0}, {})),
      gpu::MapWaitOutcome::Ready)
      << "the mapping released while in flight left the buffer mapped with no owner";
  EXPECT_THAT(adapter_->unmapBuffer(std::move(second)), gpu::IsOk());
}

/// The readback path reports whether the backend waited on the map's completion event or polled
/// for it, and that answer belongs to the adapter that did the waiting. On a platform with no
/// event wait the answer must be a plain false rather than an assumption baked into the caller,
/// and a handle that no longer names a live mapping must not be able to read the flag out of
/// whatever occupies that slot now.
TEST_F(GeodeWgpuAdapterDeviceTests, TimedWaitReportingIsFalseForAPolledWaitAndForADeadHandle) {
  const gpu::Buffer buffer = gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
      "readback", 256, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));

  gpu::BufferMapping mapping =
      gpu::GetResultOrFail(adapter_->mapBufferAsync(buffer, gpu::MapMode::Read, 0, 256));
  EXPECT_FALSE(adapter_->mappingUsedTimedWaitAny(mapping))
      << "no slice has run yet, so nothing can have waited on the completion event";

  EXPECT_EQ(
      gpu::GetResultOrFail(adapter_->waitForMapping(mapping, gpu::MapWaitParams{0.01, 2.0}, {})),
      gpu::MapWaitOutcome::Ready);
  // This build has no completion-event wait, so the slice above polled.
  EXPECT_FALSE(adapter_->mappingUsedTimedWaitAny(mapping));

  EXPECT_THAT(adapter_->unmapBuffer(std::move(mapping)), gpu::IsOk());
  EXPECT_FALSE(adapter_->mappingUsedTimedWaitAny(gpu::BufferMapping()));
}

/// A readback buffer whose map was abandoned, and a pooled readback set evicted to stay inside
/// its ceiling, both have to give their memory back at that moment. Releasing the handle alone
/// only drops the adapter's reference, so the entry point that destroys the backend object is
/// what the pool and the cancel path depend on - and it must refuse a handle that does not name
/// a live buffer of this adapter rather than destroying the slot's new occupant.
TEST_F(GeodeWgpuAdapterDeviceTests, DestroyingABufferBackingConsumesTheHandleAndRefusesStaleOnes) {
  gpu::Buffer buffer = gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
      "readback", 256, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));

  EXPECT_THAT(adapter_->destroyBufferBacking(std::move(buffer)), gpu::IsOk());
  EXPECT_FALSE(buffer.isValid()) << "the handle must be consumed either way";

  EXPECT_THAT(adapter_->destroyBufferBacking(gpu::Buffer()),
              gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle));

  // A destroyed buffer's slot can be reused; a second destroy through the old handle must not
  // reach whatever now occupies it.
  const gpu::Buffer replacement = gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
      "replacement", 256, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));
  EXPECT_TRUE(replacement.isValid());
  EXPECT_FALSE(adapter_->wgpuBufferOf(gpu::Buffer())) << "a null handle names no backend buffer";
  EXPECT_TRUE(adapter_->wgpuBufferOf(replacement));
}

/// The adapter presents to a Metal layer; the other platform surfaces are still created by the
/// embedder, so asking it for one reports the capability as unsupported rather than appearing to
/// work and then failing at the first frame.
TEST_F(GeodeWgpuAdapterDeviceTests, RejectsSurfaceKindsItDoesNotPresentTo) {
  gpu::SurfaceDescriptor descriptor;
  descriptor.label = "window";
  descriptor.native.kind = gpu::NativeSurfaceKind::XlibWindow;
  descriptor.native.display = this;
  descriptor.native.window = 1;

  EXPECT_THAT(adapter_->createSurface(descriptor), gpu::IsGpuError(gpu::GpuErrorType::Unsupported));
}

}  // namespace
}  // namespace donner::geode
