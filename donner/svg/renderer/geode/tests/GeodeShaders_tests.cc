#include "donner/svg/renderer/geode/GeodeShaders.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
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
enum class CoverageProbe {
  Endpoint,
  SingleCrossing,
  NearLinear,
  LargeQuadratic,
  SmallQuadratic,
  BelowStart,
  AtMaximum,
  InvalidControl,
  InvalidSample,
  FlatEndpoint,
  OwnedEndpoint,
  FlatOwnedEndpoint
};

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

  struct ProbeInput {
    std::array<float, 12> curves;
    std::array<float, 2> sample = {540.0f, 223.75f};
    uint32_t curveCount = 1;
  };

  /// Owners remain in the calling scope until the output has been read back.
  struct ProbePipeline {
    ScopedWgpuHandle<wgpu::ShaderModule> module;
    ScopedWgpuHandle<wgpu::ComputePipeline> pipeline;
  };

  struct ProbeBinding {
    ScopedWgpuHandle<wgpu::BindGroupLayout> layout;
    wgpu::BindGroup group;
  };

  struct ProbeOutput {
    ProbeBinding binding;
    wgpu::Texture texture;
    wgpu::Buffer readbackBuffer;
  };

  static void configureBoundaryInput(ProbeInput& input, CoverageProbe probe) {
    if (probe == CoverageProbe::BelowStart) {
      input.sample[1] = std::nextafter(223.0f, -std::numeric_limits<float>::infinity());
    } else if (probe == CoverageProbe::AtMaximum) {
      input.sample[1] = 225.0f;
    } else if (probe == CoverageProbe::InvalidControl) {
      input.curves[3] = std::numeric_limits<float>::infinity();
      input.sample[1] = 223.0f;
    } else if (probe == CoverageProbe::InvalidSample) {
      input.sample[1] = std::numeric_limits<float>::quiet_NaN();
    }
  }

  static void configureProbeInput(ProbeInput& input, CoverageProbe probe) {
    if (probe == CoverageProbe::FlatEndpoint || probe == CoverageProbe::FlatOwnedEndpoint) {
      input.curves = {627.0f, 4.0f, 627.0f, 0.0f, 627.0f, 0.0f,
                      627.0f, 0.0f, 627.0f, 0.0f, 627.0f, 4.0f};
      input.sample[1] = 0.0f;
    }
    if (probe == CoverageProbe::NearLinear || probe == CoverageProbe::LargeQuadratic ||
        probe == CoverageProbe::SmallQuadratic) {
      const float scale = probe == CoverageProbe::LargeQuadratic   ? 1e30f
                          : probe == CoverageProbe::SmallQuadratic ? 1e-25f
                                                                   : 1e-6f;
      input.curves = {627.0f, 0.0f, 627.0f, scale, 627.0f, 4.0f * scale};
      input.sample[1] = 3.0f * scale;
    } else {
      configureBoundaryInput(input, probe);
    }
  }

  static void orientProbeInput(ProbeInput& input, bool transpose, bool reverse) {
    if (reverse) {
      for (size_t first : {0u, 6u}) {
        std::swap(input.curves[first], input.curves[first + 4]);
        std::swap(input.curves[first + 1], input.curves[first + 5]);
      }
    }
    if (transpose) {
      std::swap(input.sample[0], input.sample[1]);
      for (size_t i = 0; i < input.curves.size(); i += 2) {
        std::swap(input.curves[i], input.curves[i + 1]);
      }
    }
  }

  static ProbeInput makeProbeInput(CoverageProbe probe, bool transpose, bool reverse) {
    // These are the independently rounded control points of the two line segments meeting at
    // the lightning tip in donner_splash.svg, with a horizontal closing edge between them.
    ProbeInput input{{623.76f, 226.98f, 625.67f, 225.365f, 627.58f, 223.75f, 627.56f, 223.75f,
                      625.095f, 224.725f, 622.63f, 225.7f}};
    const bool singleCrossing =
        probe != CoverageProbe::Endpoint && probe != CoverageProbe::FlatEndpoint;
    input.curveCount = singleCrossing ? 1u : 2u;
    if (singleCrossing && probe != CoverageProbe::OwnedEndpoint) {
      input.curves = {627.0f, 223.0f, 627.0f, 224.0f, 627.0f, 225.0f};
    }
    configureProbeInput(input, probe);
    orientProbeInput(input, transpose, reverse);
    return input;
  }

  static std::string coverageSource(EndpointShader shader, bool transpose) {
    std::string wgsl = source(shader);
    const std::string function = transpose ? "accumulateVert" : "accumulateHoriz";
    const std::string paint = shader == EndpointShader::Fill ? "paint, " : "";
    wgsl += R"(
@group(1) @binding(0) var endpointResult: texture_storage_2d<rgba8unorm, write>;
@group(1) @binding(1) var<storage, read> endpointSamples: array<vec2f>;
@compute @workgroup_size(1)
fn endpoint_coverage() {
)";
    if (shader == EndpointShader::Fill) {
      wgsl += "  var paint: PaintParams;\n";
    }
    wgsl += "  let ray = " + function + "(" + paint + "0u, endpointSamples[0], 2.0);\n";
    wgsl += R"(  textureStore(endpointResult, vec2i(0), vec4f(abs(ray.cov), 0.0, 0.0, 1.0));
}
)";
    return wgsl;
  }

  static ProbePipeline createProbePipeline(const std::string& wgsl) {
    const auto& runtime = device()->device();
    wgpu::ShaderSourceWGSL wgslSource{wgpu::Default};
    wgslSource.code = wgpuLabel(wgsl);
    wgpu::ShaderModuleDescriptor moduleDesc{wgpu::Default};
    moduleDesc.nextInChain = &wgslSource.chain;
    ProbePipeline result;
    result.module = ScopedWgpuHandle<wgpu::ShaderModule>(runtime.createShaderModule(moduleDesc));
    wgpu::ComputePipelineDescriptor pipelineDesc{};
    pipelineDesc.compute.module = result.module.get();
    pipelineDesc.compute.entryPoint = wgpuLabel("endpoint_coverage");
    result.pipeline =
        ScopedWgpuHandle<wgpu::ComputePipeline>(runtime.createComputePipeline(pipelineDesc));
    return result;
  }

  static wgpu::Buffer storageBuffer(ScopedWgpuResourceArena& resources, const void* data,
                                    size_t size) {
    wgpu::BufferDescriptor descriptor{};
    descriptor.size = size;
    descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
    const wgpu::Buffer buffer = resources.retain(device()->device().createBuffer(descriptor));
    device()->queue().writeBuffer(buffer, 0, data, size);
    return buffer;
  }

  static ProbeBinding createGeometryBinding(ScopedWgpuResourceArena& resources,
                                            const wgpu::ComputePipeline& pipeline,
                                            const ProbeInput& input, EndpointShader shader,
                                            bool transpose) {
    const std::array<uint32_t, 8> band = {0, input.curveCount};
    const std::array<uint32_t, 2> indices = {0, 1};
    const wgpu::Buffer bandBuffer = storageBuffer(resources, band.data(), sizeof(band));
    const wgpu::Buffer curveBuffer =
        storageBuffer(resources, input.curves.data(), sizeof(input.curves));
    const wgpu::Buffer indexBuffer = storageBuffer(resources, indices.data(), sizeof(indices));
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
    entries[1].size = sizeof(input.curves);
    entries[2].binding = indexBinding;
    entries[2].buffer = indexBuffer;
    entries[2].size = sizeof(indices);
    ProbeBinding result;
    result.layout = ScopedWgpuHandle<wgpu::BindGroupLayout>(pipeline.getBindGroupLayout(0));
    wgpu::BindGroupDescriptor groupDesc{};
    groupDesc.layout = result.layout.get();
    groupDesc.entries = entries.data();
    groupDesc.entryCount = typed ? 2 : 3;
    result.group = resources.retain(device()->device().createBindGroup(groupDesc));
    return result;
  }

  static ProbeOutput createProbeOutput(ScopedWgpuResourceArena& resources,
                                       const wgpu::ComputePipeline& pipeline,
                                       const std::array<float, 2>& sample) {
    const auto& runtime = device()->device();
    wgpu::TextureDescriptor textureDesc{};
    textureDesc.size = {1, 1, 1};
    textureDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    textureDesc.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::CopySrc;
    textureDesc.mipLevelCount = 1;
    textureDesc.sampleCount = 1;
    textureDesc.dimension = wgpu::TextureDimension::_2D;
    ProbeOutput result;
    result.texture = resources.retain(runtime.createTexture(textureDesc));
    const wgpu::Buffer sampleBuffer = storageBuffer(resources, sample.data(), sizeof(sample));
    std::array<wgpu::BindGroupEntry, 2> outputEntries{};
    outputEntries[0].binding = 0;
    outputEntries[0].textureView = resources.retain(result.texture.createView());
    outputEntries[1].binding = 1;
    outputEntries[1].buffer = sampleBuffer;
    outputEntries[1].size = sizeof(sample);
    result.binding.layout = ScopedWgpuHandle<wgpu::BindGroupLayout>(pipeline.getBindGroupLayout(1));
    wgpu::BindGroupDescriptor groupDesc{};
    groupDesc.layout = result.binding.layout.get();
    groupDesc.entries = outputEntries.data();
    groupDesc.entryCount = outputEntries.size();
    result.binding.group = resources.retain(runtime.createBindGroup(groupDesc));
    wgpu::BufferDescriptor readbackDesc{};
    readbackDesc.size = 256;
    readbackDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    result.readbackBuffer = resources.retain(runtime.createBuffer(readbackDesc));
    return result;
  }

  static svg::RendererBitmap dispatchCoverage(const wgpu::ComputePipeline& pipeline,
                                              const wgpu::BindGroup& geometry,
                                              const ProbeOutput& output) {
    const ScopedWgpuHandle<wgpu::CommandEncoder> encoder(device()->device().createCommandEncoder());
    const ScopedWgpuHandle<wgpu::ComputePassEncoder> pass(encoder.get().beginComputePass());
    pass.get().setPipeline(pipeline);
    pass.get().setBindGroup(0, geometry, 0, nullptr);
    pass.get().setBindGroup(1, output.binding.group, 0, nullptr);
    pass.get().dispatchWorkgroups(1, 1, 1);
    pass.get().end();
    wgpu::TexelCopyTextureInfo from{};
    from.texture = output.texture;
    wgpu::TexelCopyBufferInfo to{};
    to.buffer = output.readbackBuffer;
    to.layout.bytesPerRow = 256;
    to.layout.rowsPerImage = 1;
    const wgpu::Extent3D extent{1, 1, 1};
    encoder.get().copyTextureToBuffer(from, to, extent);
    const ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder.get().finish());
    device()->queue().submit(1, &commands.get());
    return svg::RendererBitmap{Vector2i(1, 1), readback(output.readbackBuffer), 4};
  }

  static svg::RendererBitmap coverage(EndpointShader shader, bool transpose, bool reverse,
                                      CoverageProbe probe = CoverageProbe::Endpoint) {
    const ProbeInput input = makeProbeInput(probe, transpose, reverse);
    const std::string wgsl = coverageSource(shader, transpose);
    ScopedWgpuResourceArena resources;
    const ProbePipeline pipeline = createProbePipeline(wgsl);
    const ProbeBinding geometry =
        createGeometryBinding(resources, pipeline.pipeline.get(), input, shader, transpose);
    const ProbeOutput output = createProbeOutput(resources, pipeline.pipeline.get(), input.sample);
    return dispatchCoverage(pipeline.pipeline.get(), geometry.group, output);
  }

  static void expectFlatTangentCancellation(EndpointShader shader) {
    for (bool transpose : {false, true}) {
      for (bool reverse : {false, true}) {
        SCOPED_TRACE(transpose);
        SCOPED_TRACE(reverse);
        const svg::RendererBitmap actual =
            coverage(shader, transpose, reverse, CoverageProbe::FlatEndpoint);
        ASSERT_THAT(actual.pixels, testing::SizeIs(4));
        const svg::RendererBitmap expected{Vector2i(1, 1), {0, 0, 0, 255}, 4};
        editor::tests::CompareBitmapToBitmap(
            actual, expected,
            "slug_flat_endpoint_" + std::to_string(static_cast<int>(shader)) + "_" +
                (transpose ? "vertical_" : "horizontal_") + (reverse ? "reversed" : "forward"),
            editor::tests::PixelmatchIdentityParams());
      }
    }
  }

  static void expectBoundaryCoverage(EndpointShader shader) {
    for (CoverageProbe probe :
         {CoverageProbe::NearLinear, CoverageProbe::LargeQuadratic, CoverageProbe::SmallQuadratic,
          CoverageProbe::BelowStart, CoverageProbe::AtMaximum, CoverageProbe::InvalidControl,
          CoverageProbe::InvalidSample, CoverageProbe::OwnedEndpoint,
          CoverageProbe::FlatOwnedEndpoint}) {
      SCOPED_TRACE(static_cast<int>(probe));
      const bool crosses =
          probe == CoverageProbe::NearLinear || probe == CoverageProbe::LargeQuadratic ||
          probe == CoverageProbe::SmallQuadratic || probe == CoverageProbe::OwnedEndpoint ||
          probe == CoverageProbe::FlatOwnedEndpoint;
      for (bool transpose : {false, true}) {
        for (bool reverse : {false, true}) {
          SCOPED_TRACE(transpose);
          SCOPED_TRACE(reverse);
          const svg::RendererBitmap actual = coverage(shader, transpose, reverse, probe);
          ASSERT_THAT(actual.pixels, testing::SizeIs(4));
          const svg::RendererBitmap expected{
              Vector2i(1, 1), {static_cast<uint8_t>(crosses ? 255 : 0), 0, 0, 255}, 4};
          const std::string label = "slug_boundary_" + std::to_string(static_cast<int>(shader)) +
                                    "_" + std::to_string(static_cast<int>(probe)) + "_" +
                                    (transpose ? "vertical_" : "horizontal_") +
                                    (reverse ? "reversed" : "forward");
          editor::tests::CompareBitmapToBitmap(actual, expected, label,
                                               editor::tests::PixelmatchIdentityParams());
        }
      }
    }
  }

  static void expectCancellingEndpoints(EndpointShader shader) {
    for (bool transpose : {false, true}) {
      for (bool reverse : {false, true}) {
        SCOPED_TRACE(transpose);
        SCOPED_TRACE(reverse);
        const svg::RendererBitmap control =
            coverage(shader, transpose, reverse, CoverageProbe::SingleCrossing);
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

TEST_F(SlugEndpointTest, TypedFillPreservesInteriorAndRejectsOutsideOrInvalidSamples) {
  expectBoundaryCoverage(EndpointShader::TypedFill);
}

TEST_F(SlugEndpointTest, FillPreservesInteriorAndRejectsOutsideOrInvalidSamples) {
  expectBoundaryCoverage(EndpointShader::Fill);
}

TEST_F(SlugEndpointTest, GradientPreservesInteriorAndRejectsOutsideOrInvalidSamples) {
  expectBoundaryCoverage(EndpointShader::Gradient);
}

TEST_F(SlugEndpointTest, MaskPreservesInteriorAndRejectsOutsideOrInvalidSamples) {
  expectBoundaryCoverage(EndpointShader::Mask);
}

TEST_F(SlugEndpointTest, TypedFillRetainsFlatTangentCrossings) {
  expectFlatTangentCancellation(EndpointShader::TypedFill);
}

TEST_F(SlugEndpointTest, FillRetainsFlatTangentCrossings) {
  expectFlatTangentCancellation(EndpointShader::Fill);
}

TEST_F(SlugEndpointTest, GradientRetainsFlatTangentCrossings) {
  expectFlatTangentCancellation(EndpointShader::Gradient);
}

TEST_F(SlugEndpointTest, MaskRetainsFlatTangentCrossings) {
  expectFlatTangentCancellation(EndpointShader::Mask);
}

}  // namespace

}  // namespace donner::geode
