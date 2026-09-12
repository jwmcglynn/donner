#include "donner/svg/renderer/geode/GeodeShaders.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "embed_resources/SlugFillWgsl.h"
#include "embed_resources/SlugGradientWgsl.h"
#include "embed_resources/SlugMaskWgsl.h"

namespace donner::geode {

/// Smoke test: the Slug fill shader compiles without errors.
/// If the WGSL has a syntax error or undefined symbol, shader module creation
/// fails and the test fails. The family creators go through the donner::gpu
/// runtime, so compilation runs on the wgpu adapter.
TEST(GeodeShaders, SlugFillCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createSlugFillShader(geodeDevice->adapterDevice());
  ASSERT_FALSE(module.hasError()) << "Slug fill shader failed to compile: " << module.error();

  // Note: Dawn's shader compilation is asynchronous in principle but for
  // WGSL it's typically synchronous. If this test passes the WGSL parsed
  // and type-checked successfully - errors would have fired the device's
  // uncaptured error callback before we get here.
}

/// Smoke test for the analytic gradient shader used on every adapter.
TEST(GeodeShaders, SlugGradientCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createSlugGradientShader(geodeDevice->adapterDevice());
  ASSERT_FALSE(module.hasError()) << "Slug gradient shader failed to compile: " << module.error();
}

/// Smoke test for the path-clip mask shader.
TEST(GeodeShaders, SlugMaskCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createSlugMaskShader(geodeDevice->adapterDevice());
  ASSERT_FALSE(module.hasError()) << "Slug mask shader failed to compile: " << module.error();
}

/// Smoke test for the image-blit shader shared by drawImage and the pattern path.
TEST(GeodeShaders, ImageBlitCompiles) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  gpu::Result<gpu::ShaderModule> module = createImageBlitShader(geodeDevice->adapterDevice());
  ASSERT_FALSE(module.hasError()) << "Image blit shader failed to compile: " << module.error();
}

namespace {

enum class EndpointShader { TypedFill, Fill, Gradient, Mask };

/// Dispatches the production coverage function at an exact shared endpoint, bypassing only
/// the rasterizer's interpolation of the sample position.
class SlugEndpointTest : public testing::Test {
protected:
  void SetUp() override { ASSERT_THAT(device(), testing::NotNull()); }

  static std::shared_ptr<GeodeDevice> device() {
    static std::shared_ptr<GeodeDevice> result = GeodeDevice::CreateHeadless();
    return result;
  }

  static std::string source(EndpointShader shader) {
    if (shader == EndpointShader::TypedFill) {
      auto module = gpu::shader::programs::BuildSolidFillModule();
      EXPECT_THAT(module.hasResult(), testing::IsTrue());
      if (!module.hasResult()) {
        return {};
      }
      auto emitted = gpu::shader::EmitWgsl(module.result());
      EXPECT_THAT(emitted.hasResult(), testing::IsTrue());
      return emitted.hasResult() ? emitted.result() : std::string();
    }
    const auto bytes = shader == EndpointShader::Fill       ? embedded::kSlugFillWgsl
                       : shader == EndpointShader::Gradient ? embedded::kSlugGradientWgsl
                                                            : embedded::kSlugMaskWgsl;
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  static std::vector<uint8_t> readback(const wgpu::Buffer& buffer) {
    struct State {
      std::atomic<bool> done = false;
      std::atomic<bool> ok = false;
    };
    auto state = std::make_shared<State>();
    wgpu::BufferMapCallbackInfo callback{wgpu::Default};
    callback.mode = wgpu::CallbackMode::AllowSpontaneous;
    callback.userdata1 = retainWgpuCallbackState(state);
    callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void*) {
      const auto result = takeWgpuCallbackState<State>(userdata);
      result->ok.store(status == WGPUMapAsyncStatus_Success, std::memory_order_relaxed);
      result->done.store(true, std::memory_order_release);
    };
    buffer.mapAsync(wgpu::MapMode::Read, 0, 256, callback);
    const GpuWaitResult waited = BoundedGpuWait(
        [&] {
          device()->device().poll(false, nullptr);
          return state->done.load(std::memory_order_acquire);
        },
        kDefaultGpuWaitTimeout);
    EXPECT_EQ(waited, GpuWaitResult::Complete);
    EXPECT_THAT(state->ok.load(std::memory_order_relaxed), testing::IsTrue());
    if (waited != GpuWaitResult::Complete || !state->ok.load(std::memory_order_relaxed)) {
      return {};
    }
    const auto* mapped = static_cast<const uint8_t*>(buffer.getConstMappedRange(0, 256));
    EXPECT_THAT(mapped, testing::NotNull());
    if (!mapped) {
      return {};
    }
    const std::vector<uint8_t> result(mapped, mapped + 4);
    buffer.unmap();
    return result;
  }

  static svg::RendererBitmap coverage(EndpointShader shader, bool transpose, bool reverse,
                                      bool singleCrossing = false) {
    // These are the independently rounded control points of the two line segments meeting at
    // the lightning tip in donner_splash.svg, with a horizontal closing edge between them.
    std::array<float, 12> curves = {623.76f, 226.98f, 625.67f,  225.365f, 627.58f, 223.75f,
                                    627.56f, 223.75f, 625.095f, 224.725f, 622.63f, 225.7f};
    if (singleCrossing) {
      curves = {627.0f, 223.0f, 627.0f, 224.0f, 627.0f, 225.0f};
    }
    if (reverse) {
      for (size_t first : {0u, 6u}) {
        std::swap(curves[first], curves[first + 4]);
        std::swap(curves[first + 1], curves[first + 5]);
      }
    }
    if (transpose) {
      for (size_t i = 0; i < curves.size(); i += 2) {
        std::swap(curves[i], curves[i + 1]);
      }
    }

    std::string wgsl = source(shader);
    const std::string sample = transpose ? "vec2f(223.75, 540.0)" : "vec2f(540.0, 223.75)";
    const std::string function = transpose ? "accumulateVert" : "accumulateHoriz";
    const std::string paint = shader == EndpointShader::Fill ? "paint, " : "";
    wgsl += R"(
@group(1) @binding(0) var endpointResult: texture_storage_2d<rgba8unorm, write>;
@compute @workgroup_size(1)
fn endpoint_coverage() {
)";
    if (shader == EndpointShader::Fill) {
      wgsl += "  var paint: PaintParams;\n";
    }
    wgsl += "  let ray = " + function + "(" + paint + "0u, " + sample + ", 2.0);\n";
    wgsl += R"(  textureStore(endpointResult, vec2i(0), vec4f(abs(ray.cov), 0.0, 0.0, 1.0));
}
)";

    ScopedWgpuResourceArena resources;
    const auto& runtime = device()->device();
    const auto& queue = device()->queue();
    wgpu::ShaderSourceWGSL wgslSource{wgpu::Default};
    wgslSource.code = wgpuLabel(wgsl);
    wgpu::ShaderModuleDescriptor moduleDesc{wgpu::Default};
    moduleDesc.nextInChain = &wgslSource.chain;
    const ScopedWgpuHandle<wgpu::ShaderModule> module(runtime.createShaderModule(moduleDesc));
    wgpu::ComputePipelineDescriptor pipelineDesc{};
    pipelineDesc.compute.module = module.get();
    pipelineDesc.compute.entryPoint = wgpuLabel("endpoint_coverage");
    const ScopedWgpuHandle<wgpu::ComputePipeline> pipeline(
        runtime.createComputePipeline(pipelineDesc));

    const auto storage = [&](const void* data, size_t size) {
      wgpu::BufferDescriptor descriptor{};
      descriptor.size = size;
      descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
      const wgpu::Buffer buffer = resources.retain(runtime.createBuffer(descriptor));
      queue.writeBuffer(buffer, 0, data, size);
      return buffer;
    };
    const std::array<uint32_t, 8> band = {0, singleCrossing ? 1u : 2u};
    const std::array<uint32_t, 2> indices = {0, 1};
    const wgpu::Buffer bandBuffer = storage(band.data(), sizeof(band));
    const wgpu::Buffer curveBuffer = storage(curves.data(), sizeof(curves));
    const wgpu::Buffer indexBuffer = storage(indices.data(), sizeof(indices));
    const bool typed = shader == EndpointShader::TypedFill;
    const bool fill = shader == EndpointShader::Fill;
    const uint32_t bandBinding = !transpose ? 1 : (typed || fill ? 8 : 5);
    const uint32_t curveBinding = bandBinding + 1;
    const uint32_t indexBinding = fill ? 10 : (transpose ? 10 : 9);
    std::array<wgpu::BindGroupEntry, 3> entries{};
    entries[0].binding = bandBinding;
    entries[0].buffer = bandBuffer;
    entries[0].size = sizeof(band);
    entries[1].binding = curveBinding;
    entries[1].buffer = curveBuffer;
    entries[1].size = sizeof(curves);
    entries[2].binding = indexBinding;
    entries[2].buffer = indexBuffer;
    entries[2].size = sizeof(indices);
    wgpu::BindGroupDescriptor groupDesc{};
    const ScopedWgpuHandle<wgpu::BindGroupLayout> geometryLayout(
        pipeline.get().getBindGroupLayout(0));
    groupDesc.layout = geometryLayout.get();
    groupDesc.entries = entries.data();
    groupDesc.entryCount = typed ? 2 : 3;
    const wgpu::BindGroup geometry = resources.retain(runtime.createBindGroup(groupDesc));

    wgpu::TextureDescriptor textureDesc{};
    textureDesc.size = {1, 1, 1};
    textureDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    textureDesc.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::CopySrc;
    textureDesc.mipLevelCount = 1;
    textureDesc.sampleCount = 1;
    textureDesc.dimension = wgpu::TextureDimension::_2D;
    const wgpu::Texture texture = resources.retain(runtime.createTexture(textureDesc));
    wgpu::BindGroupEntry outputEntry{};
    outputEntry.binding = 0;
    outputEntry.textureView = resources.retain(texture.createView());
    const ScopedWgpuHandle<wgpu::BindGroupLayout> outputLayout(
        pipeline.get().getBindGroupLayout(1));
    groupDesc.layout = outputLayout.get();
    groupDesc.entries = &outputEntry;
    groupDesc.entryCount = 1;
    const wgpu::BindGroup output = resources.retain(runtime.createBindGroup(groupDesc));
    wgpu::BufferDescriptor readbackDesc{};
    readbackDesc.size = 256;
    readbackDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    const wgpu::Buffer buffer = resources.retain(runtime.createBuffer(readbackDesc));
    const ScopedWgpuHandle<wgpu::CommandEncoder> encoder(runtime.createCommandEncoder());
    const ScopedWgpuHandle<wgpu::ComputePassEncoder> pass(encoder.get().beginComputePass());
    pass.get().setPipeline(pipeline.get());
    pass.get().setBindGroup(0, geometry, 0, nullptr);
    pass.get().setBindGroup(1, output, 0, nullptr);
    pass.get().dispatchWorkgroups(1, 1, 1);
    pass.get().end();
    wgpu::TexelCopyTextureInfo from{};
    from.texture = texture;
    wgpu::TexelCopyBufferInfo to{};
    to.buffer = buffer;
    to.layout.bytesPerRow = 256;
    to.layout.rowsPerImage = 1;
    const wgpu::Extent3D extent{1, 1, 1};
    encoder.get().copyTextureToBuffer(from, to, extent);
    const ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder.get().finish());
    queue.submit(1, &commands.get());
    return svg::RendererBitmap{Vector2i(1, 1), readback(buffer), 4};
  }

  static void expectCancellingEndpoints(EndpointShader shader) {
    for (bool transpose : {false, true}) {
      for (bool reverse : {false, true}) {
        SCOPED_TRACE(transpose);
        SCOPED_TRACE(reverse);
        const svg::RendererBitmap control = coverage(shader, transpose, reverse, true);
        ASSERT_THAT(control.pixels, testing::SizeIs(4));
        const svg::RendererBitmap controlExpected{Vector2i(1, 1), {255, 0, 0, 255}, 4};
        editor::tests::CompareBitmapToBitmap(control, controlExpected, "slug_endpoint_control",
                                             editor::tests::PixelmatchIdentityParams());
        const svg::RendererBitmap actual = coverage(shader, transpose, reverse);
        ASSERT_THAT(actual.pixels, testing::SizeIs(4));
        const svg::RendererBitmap expected{Vector2i(1, 1), {0, 0, 0, 255}, 4};
        editor::tests::CompareBitmapToBitmap(
            actual, expected,
            "slug_endpoint_" + std::to_string(static_cast<int>(shader)) + "_" +
                (transpose ? "vertical_" : "horizontal_") + (reverse ? "reversed" : "forward"),
            editor::tests::PixelmatchIdentityParams());
      }
    }
  }
};

TEST_F(SlugEndpointTest, TypedFillRetainsSharedEndpointCrossings) {
  expectCancellingEndpoints(EndpointShader::TypedFill);
}

TEST_F(SlugEndpointTest, FillRetainsSharedEndpointCrossings) {
  expectCancellingEndpoints(EndpointShader::Fill);
}

TEST_F(SlugEndpointTest, GradientRetainsSharedEndpointCrossings) {
  expectCancellingEndpoints(EndpointShader::Gradient);
}

TEST_F(SlugEndpointTest, MaskRetainsSharedEndpointCrossings) {
  expectCancellingEndpoints(EndpointShader::Mask);
}

}  // namespace

}  // namespace donner::geode
