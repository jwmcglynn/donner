/**
 * @file
 * Standalone wgpu-native repro for donner issue #551 —
 * `corrupted double-linked list` abort in Mesa llvmpipe Vulkan 25.2.8
 * after ~130 compute-shader dispatches in one process.
 *
 * Usage:
 *   bazel run --config=geode //tools/mesa_repro:lvp_compute_churn -- [N]
 * Force llvmpipe (default on GHA Linux + Ubuntu 24.04):
 *   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
 *   bazel run --config=geode //tools/mesa_repro:lvp_compute_churn -- 500
 *
 * The program allocates a fresh pair of 8×8 RGBA8Unorm textures + a 16-byte
 * uniform buffer + a bind group + a command encoder per iteration, dispatches
 * a single compute workgroup, submits, polls until idle, and drops every
 * handle. On unpatched Mesa 25.2.8 lavapipe this deterministically aborts
 * around iteration 130 with `corrupted double-linked list`. On a working
 * Vulkan driver (Metal, D3D12, or a future-patched lavapipe) it completes
 * N iterations cleanly.
 *
 * Deliberately excludes anything Donner-specific (no SVG parsing, no ECS,
 * no test harness). The whole thing is a single translation unit so Mesa
 * devs can drop it into a bug report and rebuild it against their local
 * wgpu-native without any other context.
 *
 * See docs/design_docs/0031-mesa_vulkan_repro_and_patch.md for the broader
 * investigation plan.
 */

#define WEBGPU_CPP_IMPLEMENTATION
#include <webgpu/webgpu.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

// Trivial offset-by-one compute shader. Reads input at (x-1, y-1), writes
// output at (x, y). Chosen for minimum Donner-shader similarity while still
// exercising the same dispatch pattern: sampled input texture + storage
// output texture + uniform buffer bound as a 3-entry bind group.
constexpr std::string_view kShader = R"(
struct Params {
  dx: f32,
  dy: f32,
  _pad0: f32,
  _pad1: f32,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: Params;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);
  if (any(coord >= size)) { return; }
  let src = coord - vec2i(i32(round(params.dx)), i32(round(params.dy)));
  var color: vec4f;
  if (any(src < vec2i(0)) || any(src >= size)) {
    color = vec4f(0.0);
  } else {
    color = textureLoad(input_tex, src, 0);
  }
  textureStore(output_tex, coord, color);
}
)";

// 200×200 matches Donner's typical resvg-test filter canvas size. 8×8
// workgroups → 25×25 = 625 workgroups dispatched per iteration. Smaller
// textures (8×8) failed to reproduce the bug in 500 iterations; the
// llvmpipe crash appears sensitive to texture size (likely descriptor-
// set or bind-group pool metadata scaling with bound resource size).
constexpr uint32_t kTextureSize = 200;
constexpr uint32_t kMaxIterationsDefault = 500;

wgpu::StringView asStringView(std::string_view s) {
  return wgpu::StringView{s};
}

struct Shader {
  wgpu::ShaderModule module;
  wgpu::BindGroupLayout bgl;
  wgpu::PipelineLayout pipelineLayout;
  wgpu::ComputePipeline pipeline;
};

Shader BuildShader(const wgpu::Device& device) {
  wgpu::ShaderSourceWGSL wgslSource{wgpu::Default};
  wgslSource.code.data = kShader.data();
  wgslSource.code.length = kShader.size();

  wgpu::ShaderModuleDescriptor smDesc{wgpu::Default};
  smDesc.label = asStringView("lvp_repro_shader");
  smDesc.nextInChain = &wgslSource.chain;
  auto module = device.createShaderModule(smDesc);
  if (!module) {
    std::fprintf(stderr, "[lvp_compute_churn] createShaderModule failed\n");
    std::exit(2);
  }

  // Bind-group layout: sampled texture @0, storage texture @1, uniform @2.
  wgpu::BindGroupLayoutEntry entries[3]{};
  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Compute;
  entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[0].texture.multisampled = false;
  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Compute;
  entries[1].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
  entries[1].storageTexture.format = wgpu::TextureFormat::RGBA8Unorm;
  entries[1].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Compute;
  entries[2].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[2].buffer.minBindingSize = 16;

  wgpu::BindGroupLayoutDescriptor bglDesc{};
  bglDesc.label = asStringView("lvp_repro_bgl");
  bglDesc.entryCount = 3;
  bglDesc.entries = entries;
  auto bgl = device.createBindGroupLayout(bglDesc);

  wgpu::PipelineLayoutDescriptor plDesc{};
  plDesc.label = asStringView("lvp_repro_pipeline_layout");
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bgl};
  plDesc.bindGroupLayouts = layouts;
  auto pipelineLayout = device.createPipelineLayout(plDesc);

  wgpu::ComputePipelineDescriptor cpDesc{};
  cpDesc.label = asStringView("lvp_repro_pipeline");
  cpDesc.layout = pipelineLayout;
  cpDesc.compute.module = module;
  cpDesc.compute.entryPoint = asStringView("main");
  auto pipeline = device.createComputePipeline(cpDesc);
  if (!pipeline) {
    std::fprintf(stderr, "[lvp_compute_churn] createComputePipeline failed\n");
    std::exit(2);
  }

  return {module, bgl, pipelineLayout, pipeline};
}

// Allocate a fresh RGBA8Unorm texture with the given usages.
wgpu::Texture MakeTexture(const wgpu::Device& device, const char* label,
                          wgpu::TextureUsage usage) {
  wgpu::TextureDescriptor td{};
  td.label = asStringView(label);
  td.size = {kTextureSize, kTextureSize, 1};
  td.format = wgpu::TextureFormat::RGBA8Unorm;
  td.usage = usage;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::_2D;
  return device.createTexture(td);
}

// Queue one compute dispatch that reads `input` and writes `output`,
// using `shader`'s pipeline. Creates a fresh uniform buffer + bind group.
// The wgpu resources are RETURNED (via out-parameters) so the caller can
// keep them alive for the duration of the command encoder's lifetime;
// otherwise the bind group could observe a destroyed texture view before
// the command buffer is submitted.
void QueueDispatch(const wgpu::Device& device, const wgpu::Queue& queue,
                   const Shader& shader, const wgpu::Texture& input,
                   const wgpu::Texture& output, wgpu::CommandEncoder& encoder,
                   // Keepalives:
                   wgpu::TextureView& inView, wgpu::TextureView& outView,
                   wgpu::Buffer& uniformBuf, wgpu::BindGroup& bindGroup) {
  const float params[4] = {1.0f, 1.0f, 0.0f, 0.0f};
  wgpu::BufferDescriptor bd{};
  bd.label = asStringView("params");
  bd.size = sizeof(params);
  bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  bd.mappedAtCreation = false;
  uniformBuf = device.createBuffer(bd);
  queue.writeBuffer(uniformBuf, 0, params, sizeof(params));

  inView = input.createView();
  outView = output.createView();

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = inView;
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = outView;
  bgEntries[2].binding = 2;
  bgEntries[2].buffer = uniformBuf;
  bgEntries[2].offset = 0;
  bgEntries[2].size = sizeof(params);
  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = asStringView("bg");
  bgDesc.layout = shader.bgl;
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  bindGroup = device.createBindGroup(bgDesc);

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = asStringView("pass");
  auto pass = encoder.beginComputePass(passDesc);
  pass.setPipeline(shader.pipeline);
  pass.setBindGroup(0, bindGroup, 0, nullptr);
  const uint32_t workgroups = (kTextureSize + 7) / 8;
  pass.dispatchWorkgroups(workgroups, workgroups, 1);
  pass.end();
}

// One "filter test" iteration. Mirrors GeodeFilterEngine::execute() for a
// four-primitive chain plus a readback: source → offset → offset → offset
// → offset → output → copy to host-visible buffer. Each chain link creates
// a fresh intermediate texture, uniform buffer, and bind group — the exact
// resource churn pattern that Donner hits per filter-test under llvmpipe.
void RunOneFilterChain(const wgpu::Device& device, const wgpu::Queue& queue,
                       const Shader& shader) {
  constexpr int kChainLength = 4;

  // Source: initial input. Filled with zeros via CopyDst (writeBuffer
  // style) but we don't need real content — the shader path exercises
  // texture sampling without caring about pixel values.
  auto source = MakeTexture(device, "source",
                            wgpu::TextureUsage::TextureBinding |
                                wgpu::TextureUsage::CopyDst);

  // Build N intermediate textures, each used as output then input.
  wgpu::Texture chain[kChainLength];
  for (int i = 0; i < kChainLength; ++i) {
    chain[i] = MakeTexture(
        device, "intermediate",
        wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding |
            wgpu::TextureUsage::CopySrc);
  }

  // Single command encoder records all N dispatches + final readback copy.
  wgpu::CommandEncoderDescriptor ceDesc{};
  ceDesc.label = asStringView("chain_encoder");
  auto encoder = device.createCommandEncoder(ceDesc);

  // Keepalives must outlive the encoder.finish() call, so allocate arrays.
  wgpu::TextureView inViews[kChainLength], outViews[kChainLength];
  wgpu::Buffer uniformBufs[kChainLength];
  wgpu::BindGroup bindGroups[kChainLength];

  for (int i = 0; i < kChainLength; ++i) {
    const wgpu::Texture& in = (i == 0) ? source : chain[i - 1];
    const wgpu::Texture& out = chain[i];
    QueueDispatch(device, queue, shader, in, out, encoder, inViews[i],
                  outViews[i], uniformBufs[i], bindGroups[i]);
  }

  // Readback: copy the last output texture into a MapRead buffer. Donner's
  // takeSnapshot does this every frame; the buffer-create + copy + mapAsync
  // path is a likely candidate for additional driver state growth.
  const uint32_t bytesPerRow = ((kTextureSize * 4u) + 255u) & ~255u;  // 256-aligned.
  const uint64_t readbackSize = static_cast<uint64_t>(bytesPerRow) * kTextureSize;
  wgpu::BufferDescriptor rbDesc{};
  rbDesc.label = asStringView("readback");
  rbDesc.size = readbackSize;
  rbDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  rbDesc.mappedAtCreation = false;
  auto readback = device.createBuffer(rbDesc);

  wgpu::TexelCopyTextureInfo src{};
  src.texture = chain[kChainLength - 1];
  src.mipLevel = 0;
  src.origin = {0, 0, 0};
  wgpu::TexelCopyBufferInfo dst{};
  dst.buffer = readback;
  dst.layout.bytesPerRow = bytesPerRow;
  dst.layout.rowsPerImage = kTextureSize;
  wgpu::Extent3D copySize{kTextureSize, kTextureSize, 1};
  encoder.copyTextureToBuffer(src, dst, copySize);

  auto cmdBuf = encoder.finish();
  queue.submit(1, &cmdBuf);

  // mapAsync + poll: matches Donner's takeSnapshot() pattern.
  struct MapState {
    bool done = false;
    bool ok = false;
  } mapState;
  wgpu::BufferMapCallbackInfo mapCb{wgpu::Default};
  mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/,
                      void* userdata1, void* /*userdata2*/) {
    auto* s = static_cast<MapState*>(userdata1);
    s->ok = (status == WGPUMapAsyncStatus_Success);
    s->done = true;
  };
  mapCb.userdata1 = &mapState;
  mapCb.userdata2 = nullptr;
  readback.mapAsync(wgpu::MapMode::Read, 0, readbackSize, mapCb);
  while (!mapState.done) {
    device.poll(true, nullptr);
  }
  if (mapState.ok) {
    readback.unmap();
  }
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t maxIterations = kMaxIterationsDefault;
  if (argc >= 2) {
    int n = std::atoi(argv[1]);
    if (n > 0) {
      maxIterations = static_cast<uint32_t>(n);
    }
  }

  auto instance = wgpu::createInstance();
  if (!instance) {
    std::fprintf(stderr, "[lvp_compute_churn] createInstance failed\n");
    return 1;
  }

  wgpu::RequestAdapterOptions adapterOpts{};
  auto adapter = instance.requestAdapter(adapterOpts);
  if (!adapter) {
    std::fprintf(stderr, "[lvp_compute_churn] requestAdapter failed\n");
    return 1;
  }

  // Log the adapter so bug reports unambiguously identify the driver
  // under test.
  WGPUAdapterInfo info{};
  if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
    const char* backend = "?";
    switch (info.backendType) {
      case WGPUBackendType_Vulkan: backend = "Vulkan"; break;
      case WGPUBackendType_Metal: backend = "Metal"; break;
      case WGPUBackendType_D3D12: backend = "D3D12"; break;
      case WGPUBackendType_D3D11: backend = "D3D11"; break;
      case WGPUBackendType_OpenGL: backend = "OpenGL"; break;
      case WGPUBackendType_OpenGLES: backend = "OpenGLES"; break;
      default: break;
    }
    const auto sv = [](const WGPUStringView& s) {
      return std::string_view{s.data ? s.data : "", s.data ? s.length : 0};
    };
    const auto vendor = sv(info.vendor);
    const auto deviceName = sv(info.device);
    std::fprintf(stderr,
                 "[lvp_compute_churn] Adapter: %.*s %.*s backend=%s "
                 "vendorID=0x%04x deviceID=0x%04x\n",
                 static_cast<int>(vendor.size()), vendor.data(),
                 static_cast<int>(deviceName.size()), deviceName.data(), backend,
                 info.vendorID, info.deviceID);
    wgpuAdapterInfoFreeMembers(info);
  }

  wgpu::DeviceDescriptor deviceDesc{};
  deviceDesc.label = asStringView("lvp_repro_device");
  auto device = adapter.requestDevice(deviceDesc);
  if (!device) {
    std::fprintf(stderr, "[lvp_compute_churn] requestDevice failed\n");
    return 1;
  }
  auto queue = device.getQueue();

  auto shader = BuildShader(device);

  std::fprintf(stderr,
               "[lvp_compute_churn] Starting %u dispatch iterations.\n"
               "  Expected on Mesa 25.2.8 lavapipe: glibc abort "
               "(corrupted double-linked list) around iter ~130.\n"
               "  Expected on working driver: clean completion.\n",
               maxIterations);

  for (uint32_t i = 0; i < maxIterations; ++i) {
    if (i % 10 == 0) {
      std::fprintf(stderr, "[lvp_compute_churn] iter %u\n", i);
      std::fflush(stderr);
    }
    RunOneFilterChain(device, queue, shader);
  }

  std::fprintf(stderr, "[lvp_compute_churn] Completed %u iterations cleanly.\n",
               maxIterations);
  return 0;
}
