/// @file
/// Black-box validation fixture (design 0053 transition baseline): the emitted solid-fill WGSL
/// must be accepted by the current production renderer's WebGPU runtime, both at shader module
/// creation (WGSL parse + type check) and at render pipeline creation (interface validation
/// against the same 12-entry bind group layout and vertex layout Geode uses).
///
/// The current renderer is used strictly as a black box: this test consumes GeodeDevice and the
/// public WebGPU-class API surface that Donner's own Geode code exercises, and observes failures
/// through the device's uncaptured-error stderr marker. A negative control proves the detection
/// mechanism actually fires on invalid WGSL.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

using testing::HasSubstr;
using testing::Not;

namespace donner::gpu::shader {
namespace {

/// Marker printed by GeodeDevice's uncaptured-error callback (see GeodeDevice.cc).
constexpr const char* kErrorMarker = "Uncaptured error";

/// Creates a shader module from WGSL text; mirrors Geode's own createShaderFromWgsl.
wgpu::ShaderModule CreateModuleFromWgsl(const wgpu::Device& device, const std::string& wgsl) {
  wgpu::ShaderSourceWGSL wgslSource{wgpu::Default};
  wgslSource.code.data = wgsl.data();
  wgslSource.code.length = wgsl.size();

  wgpu::ShaderModuleDescriptor desc{wgpu::Default};
  desc.nextInChain = &wgslSource.chain;
  return device.createShaderModule(desc);
}

/// Builds the solid-fill render pipeline from \p module, mirroring GeodePipeline.cc's
/// descriptor: the 12-entry bind group layout, the 20-byte vertex layout, premultiplied
/// source-over blending, and both entry points. \p binding7Visibility is Vertex for the correct
/// layout; the pipeline-time negative control passes Fragment to provoke a stage-visibility
/// mismatch with the shader's vertex-stage use of instanceTransforms.
void CreateSolidFillPipeline(const wgpu::Device& device, const wgpu::ShaderModule& module,
                             wgpu::ShaderStage binding7Visibility = wgpu::ShaderStage::Vertex) {
  wgpu::BindGroupLayoutEntry entries[12] = {};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  entries[0].buffer.type = wgpu::BufferBindingType::Uniform;

  const auto fragmentStorage = [&](int index) {
    entries[index].binding = static_cast<uint32_t>(index);
    entries[index].visibility = wgpu::ShaderStage::Fragment;
    entries[index].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  };
  fragmentStorage(1);
  fragmentStorage(2);

  entries[3].binding = 3;
  entries[3].visibility = wgpu::ShaderStage::Fragment;
  entries[3].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[3].texture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[4].binding = 4;
  entries[4].visibility = wgpu::ShaderStage::Fragment;
  entries[4].sampler.type = wgpu::SamplerBindingType::Filtering;

  entries[5].binding = 5;
  entries[5].visibility = wgpu::ShaderStage::Fragment;
  entries[5].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[5].texture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[6].binding = 6;
  entries[6].visibility = wgpu::ShaderStage::Fragment;
  entries[6].sampler.type = wgpu::SamplerBindingType::Filtering;

  entries[7].binding = 7;
  entries[7].visibility = binding7Visibility;
  entries[7].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

  fragmentStorage(8);
  fragmentStorage(9);
  fragmentStorage(10);
  fragmentStorage(11);

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.entryCount = 12;
  bglDesc.entries = entries;
  wgpu::BindGroupLayout bindGroupLayout = device.createBindGroupLayout(bglDesc);

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.createPipelineLayout(plDesc);

  wgpu::VertexAttribute vertexAttribs[3] = {};
  vertexAttribs[0].format = wgpu::VertexFormat::Float32x2;
  vertexAttribs[0].offset = 0;
  vertexAttribs[0].shaderLocation = 0;
  vertexAttribs[1].format = wgpu::VertexFormat::Float32x2;
  vertexAttribs[1].offset = 8;
  vertexAttribs[1].shaderLocation = 1;
  vertexAttribs[2].format = wgpu::VertexFormat::Uint32;
  vertexAttribs[2].offset = 16;
  vertexAttribs[2].shaderLocation = 2;

  wgpu::VertexBufferLayout vbLayout = {};
  vbLayout.arrayStride = 20;
  vbLayout.stepMode = wgpu::VertexStepMode::Vertex;
  vbLayout.attributeCount = 3;
  vbLayout.attributes = vertexAttribs;

  wgpu::BlendState blend = {};
  blend.color.srcFactor = wgpu::BlendFactor::One;
  blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
  blend.color.operation = wgpu::BlendOperation::Add;
  blend.alpha.srcFactor = wgpu::BlendFactor::One;
  blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
  blend.alpha.operation = wgpu::BlendOperation::Add;

  wgpu::ColorTargetState colorTarget = {};
  colorTarget.format = wgpu::TextureFormat::RGBA8Unorm;
  colorTarget.blend = &blend;
  colorTarget.writeMask = wgpu::ColorWriteMask::All;

  wgpu::FragmentState fragmentState = {};
  fragmentState.module = module;
  fragmentState.entryPoint = donner::geode::wgpuLabel("fs_main");
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.layout = pipelineLayout;
  rpDesc.vertex.module = module;
  rpDesc.vertex.entryPoint = donner::geode::wgpuLabel("vs_main");
  rpDesc.vertex.bufferCount = 1;
  rpDesc.vertex.buffers = &vbLayout;
  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;
  rpDesc.fragment = &fragmentState;
  rpDesc.multisample.count = 1;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  wgpu::RenderPipeline pipeline = device.createRenderPipeline(rpDesc);
  if (binding7Visibility == wgpu::ShaderStage::Vertex) {
    // Only the correct layout asserts on the handle; the sabotaged negative-control layout
    // observes failure through the uncaptured-error marker instead.
    EXPECT_TRUE(static_cast<bool>(pipeline)) << "Render pipeline creation returned null";
  }
}

TEST(WgslEmitterGeodeValidation, EmittedSolidFillPassesRendererValidation) {
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = programs::BuildSolidFillModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule = CreateModuleFromWgsl(geodeDevice->device(), wgsl.result());
  ASSERT_TRUE(static_cast<bool>(shaderModule)) << "Shader module creation returned null";
  CreateSolidFillPipeline(geodeDevice->device(), shaderModule);
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, Not(HasSubstr(kErrorMarker)))
      << "Renderer validation reported errors for the emitted WGSL";
}

TEST(WgslEmitterGeodeValidation, NegativeControlDetectsPipelineMismatch) {
  // Second negative control: pipeline-time errors must also be observable. A valid module with
  // a deliberately mismatched bind group layout (binding 7 declared fragment-only while the
  // shader reads instanceTransforms in the vertex stage) must trip the error marker at
  // createRenderPipeline.
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = programs::BuildSolidFillModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule = CreateModuleFromWgsl(geodeDevice->device(), wgsl.result());
  CreateSolidFillPipeline(geodeDevice->device(), shaderModule,
                          /*binding7Visibility=*/wgpu::ShaderStage::Fragment);
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, HasSubstr(kErrorMarker))
      << "A stage-visibility mismatch did not surface at pipeline creation; pipeline-time "
         "acceptance evidence would be meaningless";
}

TEST(WgslEmitterGeodeValidation, NegativeControlDetectsInvalidWgsl) {
  // Proves the detection mechanism: intentionally broken WGSL must trip the uncaptured-error
  // marker this fixture greps for.
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule =
      CreateModuleFromWgsl(geodeDevice->device(), "fn broken( -> nonsense { this is not wgsl }");
  (void)shaderModule;
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, HasSubstr(kErrorMarker))
      << "Invalid WGSL did not surface through the uncaptured-error callback; the positive "
         "test's acceptance evidence would be meaningless";
}

}  // namespace
}  // namespace donner::gpu::shader
