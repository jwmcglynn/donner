/// @file
/// Frozen-baseline capture tool (design 0053 phase 3): renders the shared baseline scene
/// through the CURRENT production renderer as a black box (GeodeDevice + GeoEncoder) and writes
/// the PNG the Metal vertical slice compares against.
///
/// Run on the target machine with:
///   bazel run --config=geode //donner/gpu/metal/tests:baseline_capture_tool -- \
///     $(bazel info workspace)/donner/gpu/metal/tests/testdata/solid_fill_baseline.png

#include <cstdio>
#include <vector>

#include "donner/gpu/metal/tests/BaselineScene.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/geode/GeoEncoder.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::gpu::metal::tests {
namespace {

constexpr uint32_t kBytesPerRow = kBaselineSize * 4;  // 1024; already 256-byte aligned.

int CaptureBaseline(const char* outputPath) {
  auto device = geode::GeodeDevice::CreateHeadless();
  if (!device) {
    std::fprintf(stderr, "No GPU device available for baseline capture\n");
    return 1;
  }

  geode::GeodePipeline pipeline(device->device(), wgpu::TextureFormat::RGBA8Unorm);
  geode::GeodeGradientPipeline gradientPipeline(device->device(), wgpu::TextureFormat::RGBA8Unorm);
  geode::GeodeImagePipeline imagePipeline(device->device(), wgpu::TextureFormat::RGBA8Unorm);

  wgpu::TextureDescriptor td = {};
  td.label = geode::wgpuLabel("BaselineTarget");
  td.size = {kBaselineSize, kBaselineSize, 1};
  td.format = wgpu::TextureFormat::RGBA8Unorm;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture target = device->device().createTexture(td);

  wgpu::BufferDescriptor bd = {};
  bd.label = geode::wgpuLabel("BaselineReadback");
  bd.size = kBytesPerRow * kBaselineSize;
  bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  wgpu::Buffer readback = device->device().createBuffer(bd);

  {
    geode::GeoEncoder encoder(*device, pipeline, gradientPipeline, imagePipeline, target);
    encoder.clear(css::RGBA(0, 0, 0, 0));  // Transparent background.
    encoder.setTransform(BaselinePixelFromScene());
    for (const BaselinePathSpec& spec : BaselineScenePaths()) {
      encoder.fillPath(spec.path, spec.color, spec.rule);
    }
    encoder.finish();
  }

  // Copy the render target into the mappable readback buffer.
  {
    wgpu::CommandEncoder enc = device->device().createCommandEncoder();
    wgpu::TexelCopyTextureInfo src = {};
    src.texture = target;
    src.mipLevel = 0;
    src.origin = {0, 0, 0};
    wgpu::TexelCopyBufferInfo dst = {};
    dst.buffer = readback;
    dst.layout.bytesPerRow = kBytesPerRow;
    dst.layout.rowsPerImage = kBaselineSize;
    wgpu::Extent3D copySize = {kBaselineSize, kBaselineSize, 1};
    enc.copyTextureToBuffer(src, dst, copySize);
    wgpu::CommandBuffer cmd = enc.finish();
    device->queue().submit(1, &cmd);
  }

  // Map and write the PNG.
  struct MapState {
    bool done = false;
    bool ok = false;
  } mapState;
  wgpu::BufferMapCallbackInfo mapCb{wgpu::Default};
  mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                      void* /*userdata2*/) {
    auto* state = static_cast<MapState*>(userdata1);
    state->ok = (status == WGPUMapAsyncStatus_Success);
    state->done = true;
  };
  mapCb.userdata1 = &mapState;
  mapCb.userdata2 = nullptr;
  readback.mapAsync(wgpu::MapMode::Read, 0, kBytesPerRow * kBaselineSize, mapCb);
  while (!mapState.done) {
    device->device().poll(true, nullptr);
  }
  if (!mapState.ok) {
    std::fprintf(stderr, "Readback buffer map failed\n");
    return 1;
  }

  const uint8_t* mapped =
      static_cast<const uint8_t*>(readback.getConstMappedRange(0, kBytesPerRow * kBaselineSize));
  const std::vector<uint8_t> pixels(mapped, mapped + kBytesPerRow * kBaselineSize);
  readback.unmap();

  if (!svg::RendererImageIO::writeRgbaPixelsToPngFile(outputPath, pixels, kBaselineSize,
                                                      kBaselineSize, kBaselineSize)) {
    std::fprintf(stderr, "Failed to write %s\n", outputPath);
    return 1;
  }
  std::fprintf(stderr, "Baseline written to %s\n", outputPath);
  return 0;
}

}  // namespace
}  // namespace donner::gpu::metal::tests

/// Entry point. @param argc Argument count. @param argv `argv[1]` is the output PNG path.
int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: baseline_capture_tool <output.png>\n");
    return 2;
  }
  return donner::gpu::metal::tests::CaptureBaseline(argv[1]);
}
