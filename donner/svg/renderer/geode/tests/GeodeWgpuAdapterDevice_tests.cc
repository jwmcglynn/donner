/// @file
/// Conformance tests for \c donner::geode::GeodeWgpuAdapterDevice: every resource kind the
/// solid/gradient/mask/image pipeline family needs is created through the adapter on the real
/// headless wgpu device, a minimal render pass plus both copy commands executes to completion,
/// and fail-closed inputs are rejected before reaching wgpu.

#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/tests/FloatStorageModule.h"
#include "donner/gpu/tests/FloatTextureSlice.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/SubRectangleCopyScene.h"
#include "donner/gpu/tests/SubRectangleUploadScene.h"
#include "donner/gpu/tests/VertexInputSlice.h"
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

/// Exposes only the real completion state for deterministic callback-order tests.
struct GeodeWgpuAdapterDeviceTestAccess {
  using CompletionState = GeodeWgpuAdapterDevice::CompletionState;

  static void installPendingMap(GeodeWgpuAdapterDevice& adapter, uint32_t index = 0) {
    if (adapter.slotMappings_.size() <= index) adapter.slotMappings_.resize(index + 1);
    auto& slot = adapter.slotMappings_[index];
    slot.completion = new GeodeWgpuAdapterDevice::MappingSlot::Completion();
    // This controlled completion has no backend callback retaining a second reference.
    slot.completion->references.store(1);
    slot.mapFuture.id = index + 1;
  }

  static void setTimedWait(GeodeWgpuAdapterDevice& adapter,
                           std::function<wgpu::WaitStatus()> wait) {
    adapter.timedMapWaitForTest_ = std::move(wait);
  }

  static gpu::MapSliceReport waitSliceReport(GeodeWgpuAdapterDevice& adapter) {
    return adapter.onWaitMappingSlice(0, 0.000001);
  }

  static gpu::MapSliceState waitSlice(GeodeWgpuAdapterDevice& adapter) {
    return waitSliceReport(adapter).state;
  }

  static void completeMap(GeodeWgpuAdapterDevice& adapter) {
    auto* completion = adapter.slotMappings_[0].completion;
    completion->ok.store(true, std::memory_order_relaxed);
    completion->done.store(true, std::memory_order_release);
  }

  static void abandonMap(GeodeWgpuAdapterDevice& adapter) { adapter.onUnmapBuffer(0); }

  static void setMapFuture(GeodeWgpuAdapterDevice& adapter, uint64_t id) {
    adapter.slotMappings_[0].mapFuture.id = id;
  }

  static void clearMaps(GeodeWgpuAdapterDevice& adapter) {
    adapter.timedMapWaitForTest_ = {};
    for (uint32_t index = 0; index < adapter.slotMappings_.size(); ++index) {
      adapter.onUnmapBuffer(index);
    }
  }
};

namespace {

/// Owns controlled map completions while the real adapter handles the wait and queue decision.
class TimedMapProbe {
public:
  TimedMapProbe(GeodeWgpuAdapterDevice& adapter, GeodeDevice& device)
      : adapter_(adapter), device_(device) {
    GeodeWgpuAdapterDeviceTestAccess::installPendingMap(adapter_);
    device_.setCounters(&counters);
  }
  ~TimedMapProbe() {
    GeodeWgpuAdapterDeviceTestAccess::clearMaps(adapter_);
    device_.setCounters(nullptr);
  }
  GeodeCounters counters;

private:
  GeodeWgpuAdapterDevice& adapter_;
  GeodeDevice& device_;
};

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

/// A texture of another adapter over the same backend device becomes nameable here only by being
/// registered, and the registration describes it the way its owner does rather than the way the
/// caller says. Registering it must not make this adapter responsible for the memory.
TEST_F(GeodeWgpuAdapterDeviceTests, ImportingFromASiblingAdapterNamesWhatTheOwnerNames) {
  GeodeWgpuAdapterDevice sibling(*geodeDevice_);
  const gpu::TextureDescriptor descriptor{"ownedBySibling",
                                          {8, 4},
                                          gpu::TextureFormat::RGBA8Unorm,
                                          gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc};
  gpu::Texture owned = gpu::GetResultOrFail(sibling.createTexture(descriptor));

  gpu::Texture registered = gpu::GetResultOrFail(adapter_->importTextureFrom(sibling, owned));
  EXPECT_THAT(registered.deviceId(), testing::Eq(adapter_->deviceId()));
  const gpu::TextureDescriptor registeredDescriptor =
      gpu::GetResultOrFail(adapter_->textureDescriptor(registered));
  EXPECT_THAT(registeredDescriptor.size, testing::Eq(descriptor.size));
  EXPECT_THAT(registeredDescriptor.format, testing::Eq(descriptor.format));
  EXPECT_THAT(registeredDescriptor.usage, testing::Eq(descriptor.usage));
  EXPECT_THAT(static_cast<WGPUTexture>(adapter_->wgpuTextureOf(registered)),
              testing::Eq(static_cast<WGPUTexture>(sibling.wgpuTextureOf(owned))));
  EXPECT_THAT(adapter_->ownsTextureBacking(registered), testing::IsFalse())
      << "a registration names memory this adapter did not allocate";

  // Dropping the registration forgets it and leaves the owner holding the texture.
  EXPECT_THAT(adapter_->destroyTextureBacking(std::move(registered)), gpu::IsOk());
  EXPECT_THAT(static_cast<bool>(sibling.wgpuTextureOf(owned)), testing::IsTrue());
  EXPECT_THAT(sibling.ownsTextureBacking(owned), testing::IsTrue());
}

/// Registration is what admits a texture, so it has to refuse everything nothing here could
/// sample or copy: a texture whose owner drives a different backend device, and a handle its own
/// owner no longer resolves.
TEST_F(GeodeWgpuAdapterDeviceTests, ImportingRefusesAForeignBackendAndAStaleHandle) {
  const std::unique_ptr<GeodeDevice> otherBackend = GeodeDevice::CreateHeadless();
  ASSERT_THAT(otherBackend, testing::NotNull())
      << "Failed to create a second headless wgpu device. Check driver availability.";
  GeodeWgpuAdapterDevice foreign(*otherBackend);
  const gpu::TextureDescriptor descriptor{"ownedElsewhere",
                                          {4, 4},
                                          gpu::TextureFormat::RGBA8Unorm,
                                          gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc};
  gpu::Texture onForeignBackend = gpu::GetResultOrFail(foreign.createTexture(descriptor));
  EXPECT_THAT(adapter_->importTextureFrom(foreign, onForeignBackend),
              gpu::IsGpuError(gpu::GpuErrorType::DeviceMismatch));

  GeodeWgpuAdapterDevice sibling(*geodeDevice_);
  gpu::Texture retired = gpu::GetResultOrFail(sibling.createTexture(descriptor));
  const gpu::Texture stale =
      gpu::Texture::CreateForBackend(retired.slotIndex(), retired.generation(), retired.deviceId());
  ASSERT_THAT(sibling.destroyTextureBacking(std::move(retired)), gpu::IsOk());
  EXPECT_THAT(adapter_->importTextureFrom(sibling, stale),
              gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle))
      << "a handle the owner no longer resolves must not bridge whatever now occupies its slot";
}

TEST_F(GeodeWgpuAdapterDeviceTests, MinimalLastRowUploadDoesNotReadBeyondCallerSpan) {
  const gpu::Texture texture = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"minimalUpload", gpu::Extent2d{1, 1}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc}));
  const std::array<uint8_t, 4> pixel{17, 34, 51, 68};
  ASSERT_THAT(adapter_->writeTexture(texture, pixel, gpu::TexelCopyBufferLayout{0, 256, 1},
                                     gpu::Extent2d{1, 1}),
              gpu::IsOk());

  const std::vector<uint8_t> readback =
      ReadbackTexturePixels(*geodeDevice_, adapter_->wgpuTextureOf(texture), 1);
  ASSERT_THAT(readback, Not(testing::IsEmpty()));
  EXPECT_THAT(PixelAt(readback, 0, 0), ElementsAre(17, 34, 51, 68));
}

TEST_F(GeodeWgpuAdapterDeviceTests, MinimalLastRowUploadPreservesOffsetAndMultipleRows) {
  const gpu::Texture texture = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"offsetUpload", gpu::Extent2d{2, 2}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc}));
  std::vector<uint8_t> pixels(4 + 256 + 8, 0);
  pixels[4] = 1;
  pixels[5] = 2;
  pixels[6] = 3;
  pixels[7] = 4;
  pixels[8] = 5;
  pixels[9] = 6;
  pixels[10] = 7;
  pixels[11] = 8;
  pixels[260] = 9;
  pixels[261] = 10;
  pixels[262] = 11;
  pixels[263] = 12;
  pixels[264] = 13;
  pixels[265] = 14;
  pixels[266] = 15;
  pixels[267] = 16;
  ASSERT_THAT(adapter_->writeTexture(texture, pixels, gpu::TexelCopyBufferLayout{4, 256, 2},
                                     gpu::Extent2d{2, 2}),
              gpu::IsOk());

  const std::vector<uint8_t> readback =
      ReadbackTexturePixels(*geodeDevice_, adapter_->wgpuTextureOf(texture), 2);
  ASSERT_THAT(readback, Not(testing::IsEmpty()));
  EXPECT_THAT(PixelAt(readback, 0, 0), ElementsAre(1, 2, 3, 4));
  EXPECT_THAT(PixelAt(readback, 1, 0), ElementsAre(5, 6, 7, 8));
  EXPECT_THAT(PixelAt(readback, 0, 1), ElementsAre(9, 10, 11, 12));
  EXPECT_THAT(PixelAt(readback, 1, 1), ElementsAre(13, 14, 15, 16));
}

TEST_F(GeodeWgpuAdapterDeviceTests, MinimalLastRowUploadIgnoresUnusedGiganticStride) {
  const gpu::Texture texture = gpu::GetResultOrFail(adapter_->createTexture(gpu::TextureDescriptor{
      "hugeStrideUpload", gpu::Extent2d{1, 1}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc}));
  const std::array<uint8_t, 4> pixel{21, 42, 63, 84};
  constexpr uint32_t kHugeAlignedStride =
      std::numeric_limits<uint32_t>::max() & ~(gpu::kTexelRowPitchAlignment - 1);
  ASSERT_THAT(
      adapter_->writeTexture(texture, pixel, gpu::TexelCopyBufferLayout{0, kHugeAlignedStride, 1},
                             gpu::Extent2d{1, 1}),
      gpu::IsOk());

  const std::vector<uint8_t> readback =
      ReadbackTexturePixels(*geodeDevice_, adapter_->wgpuTextureOf(texture), 1);
  ASSERT_THAT(readback, Not(testing::IsEmpty()));
  EXPECT_THAT(PixelAt(readback, 0, 0), ElementsAre(21, 42, 63, 84));
}

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

/// A host upload at a nonzero destination origin must reach wgpu with that origin intact. The
/// scene fills the whole destination with a sentinel first, so an adapter that dropped the origin
/// would land the rectangle at (0, 0) and leave the sentinel where the rectangle belongs.
TEST_F(GeodeWgpuAdapterDeviceTests, WriteTextureHonorsTheDestinationOrigin) {
  const gpu::Extent2d extent{gpu::tests::kSubRectUploadExtent, gpu::tests::kSubRectUploadExtent};
  const gpu::Texture destination = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"subRectUploadDestination", extent, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc}));

  ASSERT_THAT(
      adapter_->writeTexture(destination, gpu::tests::SubRectUploadDestinationFillBytes(),
                             gpu::TexelCopyBufferLayout{0, gpu::tests::kSubRectUploadBytesPerRow,
                                                        gpu::tests::kSubRectUploadExtent},
                             extent),
      gpu::IsOk());
  ASSERT_THAT(adapter_->writeTexture(
                  destination, gpu::tests::SubRectUploadBytes(),
                  gpu::TexelCopyBufferLayout{0, gpu::tests::kSubRectUploadBytesPerRow,
                                             gpu::tests::kSubRectUploadHeight},
                  gpu::Extent2d{gpu::tests::kSubRectUploadWidth, gpu::tests::kSubRectUploadHeight},
                  gpu::Origin2d{gpu::tests::kSubRectUploadX, gpu::tests::kSubRectUploadY}),
              gpu::IsOk());

  std::vector<uint8_t> pixels = ReadbackTexturePixels(
      *geodeDevice_, adapter_->wgpuTextureOf(destination), gpu::tests::kSubRectUploadExtent);
  ASSERT_THAT(pixels, Not(testing::IsEmpty())) << "destination readback failed";
  gpu::tests::ExpectSubRectUploadImageMatches(std::move(pixels),
                                              gpu::tests::SubRectUploadExpectedImageBytes(),
                                              "geode_adapter_upload_origin");
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

/// A frame recorded by two independent encoder owners is one submission: the adapter encodes a
/// buffer per element of the span, hands them to the queue together, and reports one serial.
/// The second buffer copies what the first one wrote, so a queue that ran them out of order
/// would copy the untouched destination instead.
TEST_F(GeodeWgpuAdapterDeviceTests, ASpanOfCommandBuffersExecutesInOrderUnderOneSerial) {
  const SubRectCopyScene scene = MakeSubRectCopyScene(*adapter_);
  const gpu::Extent2d extent{gpu::tests::kSubRectCopyExtent, gpu::tests::kSubRectCopyExtent};
  const gpu::Texture forwarded = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"subRectForwarded", extent, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc}));

  std::unique_ptr<gpu::CommandEncoder> copyEncoder =
      gpu::GetResultOrFail(adapter_->createCommandEncoder());
  RecordSubRectCopy(*copyEncoder, scene);
  std::unique_ptr<gpu::CommandEncoder> forwardEncoder =
      gpu::GetResultOrFail(adapter_->createCommandEncoder());
  EXPECT_THAT(forwardEncoder->copyTextureToTexture(scene.destination, forwarded, extent),
              gpu::IsOk());

  GeodeCounters counters;
  geodeDevice_->setCounters(&counters);

  std::array<gpu::CommandBuffer, 2> span{gpu::GetResultOrFail(copyEncoder->finish()),
                                         gpu::GetResultOrFail(forwardEncoder->finish())};
  const uint64_t serial = gpu::GetResultOrFail(adapter_->submit(span));

  EXPECT_EQ(counters.submits, 1u) << "a two-buffer span must still reach the queue once";
  EXPECT_THAT(adapter_->lastSubmittedSerial(), serial);
  ASSERT_TRUE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "submission " << serial << " did not complete";
  geodeDevice_->setCounters(nullptr);

  const std::vector<uint8_t> pixels = ReadbackTexturePixels(
      *geodeDevice_, adapter_->wgpuTextureOf(forwarded), gpu::tests::kSubRectCopyExtent);
  ASSERT_THAT(pixels, Not(testing::IsEmpty())) << "forwarded readback failed";
  ExpectSubRectCopyResult(pixels);
}

TEST_F(GeodeWgpuAdapterDeviceTests, FloatTextureDispatchPreservesSubBytePrecision) {
  const auto module = gpu::shader::BuildFloatStorageModule();
  ASSERT_FALSE(module.hasError()) << module.error();
  const auto emitted = gpu::shader::EmitWgsl(module.result());
  ASSERT_FALSE(emitted.hasError()) << emitted.error();
  gpu::tests::CheckFloatTextureStorage(
      *adapter_,
      gpu::ShaderModuleDescriptor{"float",
                                  RcString(emitted.result()),
                                  gpu::ShaderSourceKind::Wgsl,
                                  {},
                                  gpu::shader::ComputeEntryPointsOf(module.result())},
      [this](const gpu::Buffer& buffer) -> gpu::Result<std::vector<uint8_t>> {
        auto mapping = adapter_->mapBufferAsync(buffer, gpu::MapMode::Read, 0, 256);
        if (mapping.hasError()) {
          return std::move(mapping).error();
        }
        const auto wait = adapter_->waitForMapping(mapping.result(), {0.01, 2.0}, {});
        EXPECT_THAT(wait, gpu::HasResult());
        if (!wait.hasError()) {
          EXPECT_EQ(wait.result().outcome, gpu::MapWaitOutcome::Ready);
        }
        const auto bytes = adapter_->mappedBytes(mapping.result());
        std::vector<uint8_t> result;
        if (!bytes.hasError()) {
          result.assign(bytes.result().begin(), bytes.result().end());
        }
        EXPECT_THAT(adapter_->unmapBuffer(std::move(mapping).result()), gpu::IsOk());
        if (bytes.hasError()) {
          return bytes.error();
        }
        return result;
      });
}

TEST_F(GeodeWgpuAdapterDeviceTests,
       IndexedQuadsWithOffsetsInstancingAndScissorMatchTheExpectedImage) {
  const auto module = gpu::tests::BuildVertexInputModule();
  ASSERT_FALSE(module.hasError()) << module.error();
  const auto emitted = gpu::shader::EmitWgsl(module.result());
  ASSERT_FALSE(emitted.hasError()) << emitted.error();
  gpu::tests::CheckIndexedDrawScene(
      *adapter_,
      gpu::ShaderModuleDescriptor{"attributes", RcString(emitted.result()),
                                  gpu::ShaderSourceKind::Wgsl},
      [this](const gpu::Buffer& buffer) -> gpu::Result<std::vector<uint8_t>> {
        constexpr uint64_t kReadbackBytes = 256 * 12;
        auto mapping = adapter_->mapBufferAsync(buffer, gpu::MapMode::Read, 0, kReadbackBytes);
        if (mapping.hasError()) {
          return std::move(mapping).error();
        }
        const auto wait = adapter_->waitForMapping(mapping.result(), {0.01, 2.0}, {});
        EXPECT_THAT(wait, gpu::HasResult());
        if (!wait.hasError()) {
          EXPECT_EQ(wait.result().outcome, gpu::MapWaitOutcome::Ready);
        }
        const auto bytes = adapter_->mappedBytes(mapping.result());
        std::vector<uint8_t> result;
        if (!bytes.hasError()) {
          result.assign(bytes.result().begin(), bytes.result().end());
        }
        EXPECT_THAT(adapter_->unmapBuffer(std::move(mapping).result()), gpu::IsOk());
        if (bytes.hasError()) {
          return bytes.error();
        }
        return result;
      });
}

TEST_F(GeodeWgpuAdapterDeviceTests, VectorCeilAndExpRunThroughWebGpu) {
  const auto module = gpu::shader::BuildVectorCeilExpModule();
  ASSERT_FALSE(module.hasError()) << module.error();
  const auto emitted = gpu::shader::EmitWgsl(module.result());
  ASSERT_FALSE(emitted.hasError()) << emitted.error();
  gpu::tests::CheckFloatTextureStorage(
      *adapter_,
      gpu::ShaderModuleDescriptor{"float",
                                  RcString(emitted.result()),
                                  gpu::ShaderSourceKind::Wgsl,
                                  {},
                                  gpu::shader::ComputeEntryPointsOf(module.result())},
      [this](const gpu::Buffer& buffer) -> gpu::Result<std::vector<uint8_t>> {
        auto mapping = adapter_->mapBufferAsync(buffer, gpu::MapMode::Read, 0, 256);
        if (mapping.hasError()) {
          return std::move(mapping).error();
        }
        const auto wait = adapter_->waitForMapping(mapping.result(), {0.01, 2.0}, {});
        EXPECT_THAT(wait, gpu::HasResult());
        if (!wait.hasError()) {
          EXPECT_EQ(wait.result().outcome, gpu::MapWaitOutcome::Ready);
        }
        const auto bytes = adapter_->mappedBytes(mapping.result());
        std::vector<uint8_t> result;
        if (!bytes.hasError()) {
          result.assign(bytes.result().begin(), bytes.result().end());
        }
        EXPECT_THAT(adapter_->unmapBuffer(std::move(mapping).result()), gpu::IsOk());
        if (bytes.hasError()) {
          return bytes.error();
        }
        return result;
      },
      {-0.5f, 0.5f, 0.0f, 1.0f}, {0.0f, 1.0f, 16.0f, 43.0f});
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

/// A wait slice reports how it was spent, and that is what tells a readback whose completion
/// arrived through the map's own event from one that had to keep asking. The two answers cost
/// wall times orders of magnitude apart, so the statistics would be meaningless if a polled
/// fallback could report itself as an event wait.
TEST_F(GeodeWgpuAdapterDeviceTests, AWaitSliceReportsWhetherItUsedTheMapsCompletionEvent) {
  TimedMapProbe probe(*adapter_, *geodeDevice_);
  GeodeWgpuAdapterDeviceTestAccess::setTimedWait(*adapter_,
                                                 [] { return wgpu::WaitStatus::Success; });
  EXPECT_THAT(GeodeWgpuAdapterDeviceTestAccess::waitSliceReport(*adapter_),
              testing::Eq(gpu::MapSliceReport{.state = gpu::MapSliceState::Pending,
                                              .waitKind = gpu::MapWaitKind::CompletionEvent}));
}

/// Nothing completes on a lost device, so a wait for a submission serial has nothing to wait
/// for. Spending the budget anyway would cost a caller its whole deadline at the exact moment it
/// most needs to give up, and polling a lost wgpu device is what hangs on some drivers.
TEST_F(GeodeWgpuAdapterDeviceTests, WaitingForASerialOnALostDeviceGivesUpWithoutSpendingTheBudget) {
  const gpu::Buffer buffer = gpu::GetResultOrFail(adapter_->createBuffer(
      gpu::BufferDescriptor{"probe", 256, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));
  const std::array<uint8_t, 4> bytes{1, 2, 3, 4};
  ASSERT_THAT(adapter_->writeBuffer(buffer, 0, bytes), gpu::IsOk());

  geodeDevice_->markDeviceLost("serial wait test");

  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(adapter_->waitForSerial(adapter_->lastSubmittedSerial() + 1, 30.0),
              testing::IsFalse());
  EXPECT_THAT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
              Lt(1.0))
      << "a lost device can never complete the work, so the wait must not be spent on it";
}

/// Adopting a snapshot takes over a texture's allocation, so it has to tell an allocation this
/// device made from a registration whose memory belongs to the embedder. Getting that backwards
/// would either destroy someone else's texture or leak one of ours.
TEST_F(GeodeWgpuAdapterDeviceTests, OwnershipSeparatesAnAllocatedTextureFromAnImportedOne) {
  gpu::Device& runtime = *adapter_;
  gpu::Texture allocated = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"allocated",
                             {4, 4},
                             gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc}));
  gpu::Texture imported = gpu::GetResultOrFail(adapter_->importExternalTexture(
      adapter_->wgpuTextureOf(allocated), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc));

  EXPECT_THAT(runtime.ownsTextureBacking(allocated), testing::IsTrue());
  EXPECT_THAT(runtime.ownsTextureBacking(imported), testing::IsFalse())
      << "an imported registration names memory this adapter did not allocate";

  // Destroying the registration forgets it without touching the embedder's texture, which is
  // still the one the allocated handle names.
  EXPECT_THAT(runtime.destroyTextureBacking(std::move(imported)), gpu::IsOk());
  EXPECT_THAT(static_cast<bool>(adapter_->wgpuTextureOf(allocated)), testing::IsTrue());
  EXPECT_THAT(runtime.ownsTextureBacking(allocated), testing::IsTrue());

  EXPECT_THAT(runtime.destroyTextureBacking(std::move(allocated)), gpu::IsOk());
}

TEST_F(GeodeWgpuAdapterDeviceTests, TimedMapTimeoutSubmitsQueueProgressWithoutARuntimeSerial) {
  TimedMapProbe probe(*adapter_, *geodeDevice_);
  GeodeWgpuAdapterDeviceTestAccess::setTimedWait(*adapter_,
                                                 [] { return wgpu::WaitStatus::TimedOut; });
  const uint64_t beforeSerial = adapter_->lastSubmittedSerial();
  EXPECT_THAT(GeodeWgpuAdapterDeviceTestAccess::waitSlice(*adapter_),
              testing::Eq(gpu::MapSliceState::Pending));
  EXPECT_THAT(probe.counters.submits, testing::Eq(1u));
  EXPECT_THAT(adapter_->lastSubmittedSerial(), testing::Eq(beforeSerial));
}

TEST_F(GeodeWgpuAdapterDeviceTests, TimedMapCompletionDuringTimeoutNeedsNoQueueProgress) {
  TimedMapProbe probe(*adapter_, *geodeDevice_);
  GeodeWgpuAdapterDeviceTestAccess::setTimedWait(*adapter_, [&] {
    GeodeWgpuAdapterDeviceTestAccess::completeMap(*adapter_);
    return wgpu::WaitStatus::TimedOut;
  });
  EXPECT_THAT(GeodeWgpuAdapterDeviceTestAccess::waitSlice(*adapter_),
              testing::Eq(gpu::MapSliceState::Ready));
  EXPECT_THAT(probe.counters.submits, testing::Eq(0u));
}

TEST_F(GeodeWgpuAdapterDeviceTests, TimedMapReadyOrMissingFutureDoesNotRequestQueueProgress) {
  TimedMapProbe probe(*adapter_, *geodeDevice_);
  int waitCalls = 0;
  GeodeWgpuAdapterDeviceTestAccess::setTimedWait(*adapter_, [&] {
    ++waitCalls;
    return wgpu::WaitStatus::TimedOut;
  });
  GeodeWgpuAdapterDeviceTestAccess::setMapFuture(*adapter_, 0);
  EXPECT_THAT(GeodeWgpuAdapterDeviceTestAccess::waitSlice(*adapter_),
              testing::Eq(gpu::MapSliceState::Pending));
  GeodeWgpuAdapterDeviceTestAccess::completeMap(*adapter_);
  EXPECT_THAT(GeodeWgpuAdapterDeviceTestAccess::waitSlice(*adapter_),
              testing::Eq(gpu::MapSliceState::Ready));
  EXPECT_THAT(waitCalls, testing::Eq(0));
  EXPECT_THAT(probe.counters.submits, testing::Eq(0u));
}

TEST_F(GeodeWgpuAdapterDeviceTests, TimedMapSuccessfulWaitDoesNotRequestQueueProgress) {
  TimedMapProbe probe(*adapter_, *geodeDevice_);
  GeodeWgpuAdapterDeviceTestAccess::setTimedWait(*adapter_,
                                                 [] { return wgpu::WaitStatus::Success; });
  EXPECT_THAT(GeodeWgpuAdapterDeviceTestAccess::waitSlice(*adapter_),
              testing::Eq(gpu::MapSliceState::Pending));
  EXPECT_THAT(probe.counters.submits, testing::Eq(0u));
}

TEST_F(GeodeWgpuAdapterDeviceTests, TimedMapDeviceLossDuringWaitDoesNotSubmit) {
  TimedMapProbe probe(*adapter_, *geodeDevice_);
  GeodeWgpuAdapterDeviceTestAccess::setTimedWait(*adapter_, [&] {
    geodeDevice_->markDeviceLost("map-wait test");
    return wgpu::WaitStatus::TimedOut;
  });
  EXPECT_THAT(GeodeWgpuAdapterDeviceTestAccess::waitSlice(*adapter_),
              testing::Eq(gpu::MapSliceState::DeviceLost));
  EXPECT_THAT(probe.counters.submits, testing::Eq(0u));
}

TEST_F(GeodeWgpuAdapterDeviceTests, TimedMapAbandonmentDuringWaitRetainsCompletionUntilReturn) {
  TimedMapProbe probe(*adapter_, *geodeDevice_);
  GeodeWgpuAdapterDeviceTestAccess::setTimedWait(*adapter_, [&] {
    GeodeWgpuAdapterDeviceTestAccess::abandonMap(*adapter_);
    return wgpu::WaitStatus::TimedOut;
  });
  EXPECT_THAT(GeodeWgpuAdapterDeviceTestAccess::waitSlice(*adapter_),
              testing::Eq(gpu::MapSliceState::Failed));
  EXPECT_THAT(probe.counters.submits, testing::Eq(0u));
}

TEST_F(GeodeWgpuAdapterDeviceTests, TimedMapSlotReuseDoesNotSubmitForTheReplacement) {
  TimedMapProbe probe(*adapter_, *geodeDevice_);
  GeodeWgpuAdapterDeviceTestAccess::setTimedWait(*adapter_, [&] {
    GeodeWgpuAdapterDeviceTestAccess::abandonMap(*adapter_);
    GeodeWgpuAdapterDeviceTestAccess::installPendingMap(*adapter_);
    GeodeWgpuAdapterDeviceTestAccess::setMapFuture(*adapter_, 2);
    return wgpu::WaitStatus::TimedOut;
  });
  EXPECT_THAT(GeodeWgpuAdapterDeviceTestAccess::waitSlice(*adapter_),
              testing::Eq(gpu::MapSliceState::Failed));
  EXPECT_THAT(probe.counters.submits, testing::Eq(0u));
}

TEST_F(GeodeWgpuAdapterDeviceTests, TimedMapSlotGrowthReacquiresTheLiveMapping) {
  TimedMapProbe probe(*adapter_, *geodeDevice_);
  GeodeWgpuAdapterDeviceTestAccess::setTimedWait(*adapter_, [&] {
    GeodeWgpuAdapterDeviceTestAccess::installPendingMap(*adapter_, 1024);
    return wgpu::WaitStatus::TimedOut;
  });
  EXPECT_THAT(GeodeWgpuAdapterDeviceTestAccess::waitSlice(*adapter_),
              testing::Eq(gpu::MapSliceState::Pending));
  EXPECT_THAT(probe.counters.submits, testing::Eq(1u));
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
      gpu::GetResultOrFail(adapter_->waitForMapping(second, gpu::MapWaitParams{0.01, 2.0}, {}))
          .outcome,
      gpu::MapWaitOutcome::Ready)
      << "the mapping released while in flight left the buffer mapped with no owner";
  EXPECT_THAT(adapter_->unmapBuffer(std::move(second)), gpu::IsOk());
}

/// The readback path reports whether the backend waited on the map's completion event or polled
/// for it, and that answer belongs to the backend that did the waiting. On a platform with no
/// event wait the wait must say it polled, rather than the caller assuming either answer.
TEST_F(GeodeWgpuAdapterDeviceTests, AWaitWithNoEventToWaitOnReportsThatItPolled) {
  const gpu::Buffer buffer = gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
      "readback", 256, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));

  gpu::BufferMapping mapping =
      gpu::GetResultOrFail(adapter_->mapBufferAsync(buffer, gpu::MapMode::Read, 0, 256));

  // This build has no completion-event wait, so every slice of this wait polled.
  EXPECT_THAT(
      gpu::GetResultOrFail(adapter_->waitForMapping(mapping, gpu::MapWaitParams{0.01, 2.0}, {})),
      testing::Eq(gpu::MapWaitReport{.outcome = gpu::MapWaitOutcome::Ready,
                                     .waitKind = gpu::MapWaitKind::Polled}));

  EXPECT_THAT(adapter_->unmapBuffer(std::move(mapping)), gpu::IsOk());
}

/// The browser waits out a map slice on the map's completion event, and that wait returns for
/// two reasons: the map finished, or the slice expired with the map still pending. The second
/// case must report "not finished" so the caller waits again - reporting it as a failed map ends
/// the readback on its first slice, the snapshot comes back empty, and the editor render that
/// asked for it has nothing to present.
///
/// That arm is compiled out everywhere these suites run, which is how it reached the browser
/// lane unchecked, so the seam below stands in for it.
TEST_F(GeodeWgpuAdapterDeviceTests, AnEventWaitThatLearnedNothingReportsPendingNotFailed) {
  const gpu::Buffer buffer = gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
      "readback", 256, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));

  gpu::BufferMapping mapping =
      gpu::GetResultOrFail(adapter_->mapBufferAsync(buffer, gpu::MapMode::Read, 0, 256));

  // Nothing has polled since the map was requested, so the map cannot already be complete.
  adapter_->setSimulateEventWaitForTest(true);
  const gpu::Result<gpu::MapWaitReport> expired =
      adapter_->waitForMapping(mapping, gpu::MapWaitParams{0.0001, 0.0001}, {});
  adapter_->setSimulateEventWaitForTest(false);

  ASSERT_THAT(expired, gpu::HasResult());
  EXPECT_EQ(expired.result().outcome, gpu::MapWaitOutcome::TimedOut)
      << "a slice that waited and learned nothing must leave the map waitable, not report it "
         "failed";

  // The mapping is still usable: a real wait now completes it.
  EXPECT_EQ(
      gpu::GetResultOrFail(adapter_->waitForMapping(mapping, gpu::MapWaitParams{0.01, 2.0}, {}))
          .outcome,
      gpu::MapWaitOutcome::Ready);
  EXPECT_THAT(adapter_->unmapBuffer(std::move(mapping)), gpu::IsOk());
}

/// A readback buffer whose map was abandoned, and a pooled readback set evicted to stay inside
/// its ceiling, both have to give their memory back at that moment. Releasing the handle alone
/// only drops the adapter's reference, so the entry point that destroys the backend object is
/// what the pool and the cancel path depend on - and it must refuse a handle that does not name
/// a live buffer of this adapter rather than destroying the slot's new occupant.
TEST_F(GeodeWgpuAdapterDeviceTests, DestroyingABufferBackingConsumesTheHandleAndRefusesStaleOnes) {
  gpu::Buffer buffer = gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
      "readback", 256, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));
  const uint32_t destroyedSlot = buffer.slotIndex();
  const uint32_t destroyedGeneration = buffer.generation();
  const uint64_t destroyedDeviceId = buffer.deviceId();

  EXPECT_THAT(adapter_->destroyBufferBacking(std::move(buffer)), gpu::IsOk());
  EXPECT_FALSE(buffer.isValid()) << "the handle must be consumed either way";

  EXPECT_THAT(adapter_->destroyBufferBacking(gpu::Buffer()),
              gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle));

  // A destroyed buffer's slot gets reused, so the interesting probe is a handle that still names
  // that slot with the OLD generation - not the consumed handle, which is null and would only
  // retest the null check above. Rebuilt from the identity captured before the destroy, and
  // inert (no device-alive token) so it never self-releases.
  gpu::Buffer replacement = gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
      "replacement", 256, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));
  ASSERT_TRUE(replacement.isValid());
  ASSERT_EQ(replacement.slotIndex(), destroyedSlot)
      << "the probe is only meaningful if the replacement took the destroyed buffer's slot";

  gpu::Buffer stale =
      gpu::Buffer::CreateForBackend(destroyedSlot, destroyedGeneration, destroyedDeviceId);
  EXPECT_THAT(adapter_->destroyBufferBacking(std::move(stale)),
              gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle))
      << "a stale generation must not destroy the slot's new occupant";
  // Still live: destroying it now succeeds, which it could not if the stale handle had taken it.
  EXPECT_THAT(adapter_->destroyBufferBacking(std::move(replacement)), gpu::IsOk())
      << "the replacement must have survived the stale destroy";
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
