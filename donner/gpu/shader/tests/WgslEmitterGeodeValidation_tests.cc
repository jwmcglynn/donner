/// @file
/// Black-box validation fixture: emitted WGSL must be accepted by the current production
/// renderer's WebGPU runtime, both at shader module creation (WGSL parse + type check) and at
/// pipeline creation (interface validation against the same bind group and vertex layouts Geode
/// uses).
///
/// The current renderer is used strictly as a black box: this test consumes GeodeDevice and the
/// public WebGPU-class API surface that Donner's own Geode code exercises, and observes failures
/// through the device's uncaptured-error stderr marker. A negative control proves the detection
/// mechanism actually fires on invalid WGSL.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/ColorMatrix.h"
#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/tests/MathPrimitiveCoverageModule.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
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

/// Builds the color-matrix compute pipeline from \p module, mirroring the bind group layout the
/// program declares: sampled input texture, write-only rgba8unorm storage output, uniform
/// params, and a read-only storage bias buffer, all compute-visible.
/// @param device WebGPU device to create through.
/// @param module Shader module holding the `cs_main` entry point.
/// @param outputAccess Storage-texture access declared for binding 1; the negative control
///   passes ReadOnly to provoke an access mismatch with the shader's write-only declaration.
void CreateColorMatrixComputePipeline(
    const wgpu::Device& device, const wgpu::ShaderModule& module,
    wgpu::StorageTextureAccess outputAccess = wgpu::StorageTextureAccess::WriteOnly) {
  wgpu::BindGroupLayoutEntry entries[4] = {};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Compute;
  entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Compute;
  entries[1].storageTexture.access = outputAccess;
  entries[1].storageTexture.format = wgpu::TextureFormat::RGBA8Unorm;
  entries[1].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Compute;
  entries[2].buffer.type = wgpu::BufferBindingType::Uniform;

  entries[3].binding = 3;
  entries[3].visibility = wgpu::ShaderStage::Compute;
  entries[3].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.entryCount = 4;
  bglDesc.entries = entries;
  wgpu::BindGroupLayout bindGroupLayout = device.createBindGroupLayout(bglDesc);

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.createPipelineLayout(plDesc);

  wgpu::ComputePipelineDescriptor cpDesc = {};
  cpDesc.layout = pipelineLayout;
  cpDesc.compute.module = module;
  cpDesc.compute.entryPoint = donner::geode::wgpuLabel("cs_main");

  wgpu::ComputePipeline pipeline = device.createComputePipeline(cpDesc);
  if (outputAccess == wgpu::StorageTextureAccess::WriteOnly) {
    // Only the correct layout asserts on the handle; the sabotaged negative-control layout
    // observes failure through the uncaptured-error marker instead.
    EXPECT_TRUE(static_cast<bool>(pipeline)) << "Compute pipeline creation returned null";
  }
}

TEST(WgslEmitterGeodeValidation, EmittedColorMatrixComputePassesRendererValidation) {
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = programs::BuildColorMatrixModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule = CreateModuleFromWgsl(geodeDevice->device(), wgsl.result());
  ASSERT_TRUE(static_cast<bool>(shaderModule)) << "Shader module creation returned null";
  CreateColorMatrixComputePipeline(geodeDevice->device(), shaderModule);
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, Not(HasSubstr(kErrorMarker)))
      << "Renderer validation reported errors for the emitted compute WGSL";
}

TEST(WgslEmitterGeodeValidation, NegativeControlDetectsComputeStorageAccessMismatch) {
  // Compute-pipeline errors must also be observable: a layout declaring the output storage
  // texture read-only contradicts the shader's write-only declaration and must trip the marker.
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = programs::BuildColorMatrixModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule = CreateModuleFromWgsl(geodeDevice->device(), wgsl.result());
  CreateColorMatrixComputePipeline(geodeDevice->device(), shaderModule,
                                   /*outputAccess=*/wgpu::StorageTextureAccess::ReadOnly);
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, HasSubstr(kErrorMarker))
      << "A storage-texture access mismatch did not surface at compute pipeline creation; "
         "pipeline-time acceptance evidence would be meaningless";
}

/// Builds the flood compute pipeline from \p module, mirroring the bind group layout the program
/// declares: a write-only rgba8unorm storage output and a uniform params buffer, both
/// compute-visible. The program reads no texture, so a layout for it declares no sampled entry.
/// @param device WebGPU device to create through.
/// @param module Shader module holding the `cs_main` entry point.
/// @param outputAccess Storage-texture access declared for binding 0; the negative control passes
///   ReadOnly to provoke an access mismatch with the shader's write-only declaration.
void CreateFloodComputePipeline(
    const wgpu::Device& device, const wgpu::ShaderModule& module,
    wgpu::StorageTextureAccess outputAccess = wgpu::StorageTextureAccess::WriteOnly) {
  wgpu::BindGroupLayoutEntry entries[2] = {};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Compute;
  entries[0].storageTexture.access = outputAccess;
  entries[0].storageTexture.format = wgpu::TextureFormat::RGBA8Unorm;
  entries[0].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Compute;
  entries[1].buffer.type = wgpu::BufferBindingType::Uniform;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.entryCount = 2;
  bglDesc.entries = entries;
  wgpu::BindGroupLayout bindGroupLayout = device.createBindGroupLayout(bglDesc);

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.createPipelineLayout(plDesc);

  wgpu::ComputePipelineDescriptor cpDesc = {};
  cpDesc.layout = pipelineLayout;
  cpDesc.compute.module = module;
  cpDesc.compute.entryPoint = donner::geode::wgpuLabel("cs_main");

  wgpu::ComputePipeline pipeline = device.createComputePipeline(cpDesc);
  if (outputAccess == wgpu::StorageTextureAccess::WriteOnly) {
    // Only the correct layout asserts on the handle; the sabotaged negative-control layout
    // observes failure through the uncaptured-error marker instead.
    EXPECT_TRUE(static_cast<bool>(pipeline)) << "Compute pipeline creation returned null";
  }
}

/// Builds the subregion-clip compute pipeline from \p module, mirroring the bind group layout the
/// program declares: sampled input texture, write-only rgba8unorm storage output, and uniform
/// params, all compute-visible.
/// @param device WebGPU device to create through.
/// @param module Shader module holding the `cs_main` entry point.
/// @param inputSampleType Sample type declared for binding 0; the negative control passes
///   Uint to provoke a mismatch with the shader's texture_2d<f32> declaration.
void CreateSubregionClipComputePipeline(
    const wgpu::Device& device, const wgpu::ShaderModule& module,
    wgpu::TextureSampleType inputSampleType = wgpu::TextureSampleType::Float) {
  wgpu::BindGroupLayoutEntry entries[3] = {};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Compute;
  entries[0].texture.sampleType = inputSampleType;
  entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Compute;
  entries[1].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
  entries[1].storageTexture.format = wgpu::TextureFormat::RGBA8Unorm;
  entries[1].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Compute;
  entries[2].buffer.type = wgpu::BufferBindingType::Uniform;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.entryCount = 3;
  bglDesc.entries = entries;
  wgpu::BindGroupLayout bindGroupLayout = device.createBindGroupLayout(bglDesc);

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.createPipelineLayout(plDesc);

  wgpu::ComputePipelineDescriptor cpDesc = {};
  cpDesc.layout = pipelineLayout;
  cpDesc.compute.module = module;
  cpDesc.compute.entryPoint = donner::geode::wgpuLabel("cs_main");

  wgpu::ComputePipeline pipeline = device.createComputePipeline(cpDesc);
  if (inputSampleType == wgpu::TextureSampleType::Float) {
    // Only the correct layout asserts on the handle; the sabotaged negative-control layout
    // observes failure through the uncaptured-error marker instead.
    EXPECT_TRUE(static_cast<bool>(pipeline)) << "Compute pipeline creation returned null";
  }
}

TEST(WgslEmitterGeodeValidation, EmittedFloodComputePassesRendererValidation) {
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = programs::BuildFloodModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule = CreateModuleFromWgsl(geodeDevice->device(), wgsl.result());
  ASSERT_TRUE(static_cast<bool>(shaderModule)) << "Shader module creation returned null";
  CreateFloodComputePipeline(geodeDevice->device(), shaderModule);
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, Not(HasSubstr(kErrorMarker)))
      << "Renderer validation reported errors for the emitted compute WGSL";
}

TEST(WgslEmitterGeodeValidation, NegativeControlDetectsFloodStorageAccessMismatch) {
  // The flood layout has no sampled entry to get wrong, so its detection evidence is the storage
  // access: a read-only declaration contradicts the shader's write-only one and must trip the
  // marker.
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = programs::BuildFloodModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule = CreateModuleFromWgsl(geodeDevice->device(), wgsl.result());
  CreateFloodComputePipeline(geodeDevice->device(), shaderModule,
                             /*outputAccess=*/wgpu::StorageTextureAccess::ReadOnly);
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, HasSubstr(kErrorMarker))
      << "A storage-texture access mismatch did not surface at compute pipeline creation; "
         "pipeline-time acceptance evidence would be meaningless";
}

TEST(WgslEmitterGeodeValidation, EmittedSubregionClipComputePassesRendererValidation) {
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = programs::BuildSubregionClipModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule = CreateModuleFromWgsl(geodeDevice->device(), wgsl.result());
  ASSERT_TRUE(static_cast<bool>(shaderModule)) << "Shader module creation returned null";
  CreateSubregionClipComputePipeline(geodeDevice->device(), shaderModule);
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, Not(HasSubstr(kErrorMarker)))
      << "Renderer validation reported errors for the emitted compute WGSL";
}

TEST(WgslEmitterGeodeValidation, NegativeControlDetectsSubregionClipSampleTypeMismatch) {
  // The detection evidence for this layout is its sampled entry: declaring the source as an
  // integer texture contradicts the shader's texture_2d<f32> and must trip the marker.
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = programs::BuildSubregionClipModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule = CreateModuleFromWgsl(geodeDevice->device(), wgsl.result());
  CreateSubregionClipComputePipeline(geodeDevice->device(), shaderModule,
                                     /*inputSampleType=*/wgpu::TextureSampleType::Uint);
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, HasSubstr(kErrorMarker))
      << "A sampled-texture type mismatch did not surface at compute pipeline creation; "
         "pipeline-time acceptance evidence would be meaningless";
}

/// Builds the color-matrix filter compute pipeline from \p module against a layout written for
/// that program rather than borrowed from another with the same binding shape, so a pipeline
/// miswired around this program's own uniform block, workgroup size, or entry point is caught.
/// @param device WebGPU device to create through.
/// @param module Shader module holding the `cs_main` entry point.
/// @param outputAccess Storage-texture access declared for binding 1; the negative control passes
///   ReadOnly to provoke an access mismatch with the shader's write-only declaration.
void CreateFilterColorMatrixComputePipeline(
    const wgpu::Device& device, const wgpu::ShaderModule& module,
    wgpu::StorageTextureAccess outputAccess = wgpu::StorageTextureAccess::WriteOnly) {
  wgpu::BindGroupLayoutEntry entries[3] = {};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Compute;
  entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Compute;
  entries[1].storageTexture.access = outputAccess;
  entries[1].storageTexture.format = wgpu::TextureFormat::RGBA8Unorm;
  entries[1].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Compute;
  entries[2].buffer.type = wgpu::BufferBindingType::Uniform;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.entryCount = 3;
  bglDesc.entries = entries;
  wgpu::BindGroupLayout bindGroupLayout = device.createBindGroupLayout(bglDesc);

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.createPipelineLayout(plDesc);

  wgpu::ComputePipelineDescriptor cpDesc = {};
  cpDesc.layout = pipelineLayout;
  cpDesc.compute.module = module;
  cpDesc.compute.entryPoint = donner::geode::wgpuLabel("cs_main");

  wgpu::ComputePipeline pipeline = device.createComputePipeline(cpDesc);
  if (outputAccess == wgpu::StorageTextureAccess::WriteOnly) {
    // Only the correct layout asserts on the handle; the sabotaged negative-control layout
    // observes failure through the uncaptured-error marker instead.
    EXPECT_TRUE(static_cast<bool>(pipeline)) << "Compute pipeline creation returned null";
  }
}

TEST(WgslEmitterGeodeValidation, EmittedFilterColorMatrixPassesRendererValidation) {
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = programs::BuildFilterColorMatrixModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule = CreateModuleFromWgsl(geodeDevice->device(), wgsl.result());
  ASSERT_TRUE(static_cast<bool>(shaderModule)) << "Shader module creation returned null";
  CreateFilterColorMatrixComputePipeline(geodeDevice->device(), shaderModule);
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, Not(HasSubstr(kErrorMarker)))
      << "Renderer validation reported errors for the emitted compute WGSL";
}

TEST(WgslEmitterGeodeValidation, NegativeControlDetectsFilterColorMatrixStorageAccessMismatch) {
  // The acceptance above is only evidence if this layout can report a fault at all: a read-only
  // declaration of the destination contradicts the shader's write-only one and must trip the
  // marker.
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = programs::BuildFilterColorMatrixModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  testing::internal::CaptureStderr();
  wgpu::ShaderModule shaderModule = CreateModuleFromWgsl(geodeDevice->device(), wgsl.result());
  CreateFilterColorMatrixComputePipeline(geodeDevice->device(), shaderModule,
                                         /*outputAccess=*/wgpu::StorageTextureAccess::ReadOnly);
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_THAT(errors, HasSubstr(kErrorMarker))
      << "A storage-texture access mismatch did not surface at compute pipeline creation; "
         "pipeline-time acceptance evidence would be meaningless";
}

/// Encodes \p value the way the math-primitive module encodes a signed result into a texel: the
/// bias keeps the whole range positive, and 255ths are exactly representable in rgba8unorm.
/// @param value Signed result to encode.
uint8_t EncodeSignedResult(float value) {
  return static_cast<uint8_t>(std::lround(value + kMathPrimitiveSignedBias));
}

/// Byte the module writes to the sign channel for \p value.
/// @param value Input value the sign was taken of.
uint8_t EncodeSignResult(float value) {
  const float sign = (value > 0.0f) ? 1.0f : ((value < 0.0f) ? -1.0f : 0.0f);
  return static_cast<uint8_t>(
      std::lround((sign * kMathPrimitiveSignScale) + kMathPrimitiveSignScale));
}

/// The module's pow channel evaluated on the host.
/// @param value Input value.
float PowChannelOnHost(float value) {
  const float straight = std::clamp(value, 0.0f, 1.0f);
  const float linearized = std::pow((straight + 0.055f) / 1.055f, 2.4f);
  return 0.25f * (linearized + std::pow(straight, 2.4f) + std::pow(1.0f - straight, 2.4f));
}

/// Runs the math-primitive compute module over \p values on the renderer's own device and
/// returns the destination texels, four bytes per input. Returns an empty vector on any GPU-side
/// failure, which the caller reports.
/// @param device WebGPU device to run on.
/// @param queue Queue to submit on.
/// @param wgsl Emitted WGSL for the module.
/// @param values Input values, one per invocation.
std::vector<uint8_t> RunMathPrimitiveModule(const wgpu::Device& device, const wgpu::Queue& queue,
                                            const std::string& wgsl,
                                            const std::vector<float>& values) {
  const uint32_t laneCount = static_cast<uint32_t>(values.size());

  wgpu::BindGroupLayoutEntry layoutEntries[2] = {};
  layoutEntries[0].binding = 0;
  layoutEntries[0].visibility = wgpu::ShaderStage::Compute;
  layoutEntries[0].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
  layoutEntries[0].storageTexture.format = wgpu::TextureFormat::RGBA8Unorm;
  layoutEntries[0].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;
  layoutEntries[1].binding = 1;
  layoutEntries[1].visibility = wgpu::ShaderStage::Compute;
  layoutEntries[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.entryCount = 2;
  bglDesc.entries = layoutEntries;
  wgpu::BindGroupLayout bindGroupLayout = device.createBindGroupLayout(bglDesc);

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.createPipelineLayout(plDesc);

  wgpu::ComputePipelineDescriptor cpDesc = {};
  cpDesc.layout = pipelineLayout;
  cpDesc.compute.module = CreateModuleFromWgsl(device, wgsl);
  cpDesc.compute.entryPoint = donner::geode::wgpuLabel("cs_main");
  wgpu::ComputePipeline pipeline = device.createComputePipeline(cpDesc);
  if (!pipeline) {
    return {};
  }

  wgpu::BufferDescriptor inputDesc = {};
  inputDesc.size = values.size() * sizeof(float);
  inputDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer inputBuffer = device.createBuffer(inputDesc);
  queue.writeBuffer(inputBuffer, 0, values.data(), inputDesc.size);

  wgpu::TextureDescriptor texDesc = {};
  texDesc.size = {laneCount, 1, 1};
  texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  texDesc.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::CopySrc;
  texDesc.mipLevelCount = 1;
  texDesc.sampleCount = 1;
  texDesc.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture output = device.createTexture(texDesc);
  wgpu::TextureView outputView = output.createView();

  wgpu::BindGroupEntry bgEntries[2] = {};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = outputView;
  bgEntries[1].binding = 1;
  bgEntries[1].buffer = inputBuffer;
  bgEntries[1].offset = 0;
  bgEntries[1].size = inputDesc.size;

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.layout = bindGroupLayout;
  bgDesc.entryCount = 2;
  bgDesc.entries = bgEntries;
  wgpu::BindGroup bindGroup = device.createBindGroup(bgDesc);

  // The readback row pitch is the WebGPU-mandated 256-byte multiple, so the destination width is
  // read out of the padded row rather than assumed to fill it.
  constexpr uint32_t kBytesPerRow = 256;
  wgpu::BufferDescriptor readbackDesc = {};
  readbackDesc.size = kBytesPerRow;
  readbackDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  wgpu::Buffer readback = device.createBuffer(readbackDesc);

  wgpu::CommandEncoder encoder = device.createCommandEncoder();
  {
    wgpu::ComputePassDescriptor passDesc = {};
    wgpu::ComputePassEncoder pass = encoder.beginComputePass(passDesc);
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, bindGroup, 0, nullptr);
    pass.dispatchWorkgroups(laneCount / kMathPrimitiveWorkgroupSize, 1, 1);
    pass.end();
  }

  wgpu::TexelCopyTextureInfo src = {};
  src.texture = output;
  src.mipLevel = 0;
  src.origin = {0, 0, 0};

  wgpu::TexelCopyBufferInfo dst = {};
  dst.buffer = readback;
  dst.layout.bytesPerRow = kBytesPerRow;
  dst.layout.rowsPerImage = 1;

  wgpu::Extent3D copySize = {laneCount, 1, 1};
  encoder.copyTextureToBuffer(src, dst, copySize);

  wgpu::CommandBuffer commands = encoder.finish();
  queue.submit(1, &commands);

  struct MapState {
    std::atomic<bool> done = false;
    std::atomic<bool> ok = false;
  };
  auto mapState = std::make_shared<MapState>();
  wgpu::BufferMapCallbackInfo mapCb{wgpu::Default};
  mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                      void* /*userdata2*/) {
    const std::shared_ptr<MapState> state =
        donner::geode::takeWgpuCallbackState<MapState>(userdata1);
    state->ok.store(status == WGPUMapAsyncStatus_Success, std::memory_order_relaxed);
    state->done.store(true, std::memory_order_release);
  };
  mapCb.userdata1 = donner::geode::retainWgpuCallbackState(mapState);
  mapCb.userdata2 = nullptr;
  readback.mapAsync(wgpu::MapMode::Read, 0, kBytesPerRow, mapCb);

  const donner::geode::GpuWaitResult waitResult = donner::geode::BoundedGpuWait(
      [&] {
        device.poll(false, nullptr);
        return mapState->done.load(std::memory_order_acquire);
      },
      donner::geode::kDefaultGpuWaitTimeout);
  if (waitResult != donner::geode::GpuWaitResult::Complete ||
      !mapState->ok.load(std::memory_order_relaxed)) {
    return {};
  }

  const uint8_t* mapped =
      static_cast<const uint8_t*>(readback.getConstMappedRange(0, kBytesPerRow));
  if (mapped == nullptr) {
    return {};
  }
  std::vector<uint8_t> texels(mapped, mapped + size_t{laneCount} * 4u);
  readback.unmap();
  return texels;
}

TEST(WgslEmitterGeodeValidation, RoundHalfAwayFromZeroRunsOnTheDeviceAndMatchesTheCpuPath) {
  // The composition sign(x) * floor(abs(x) + 0.5) is round-half-away-from-zero, which is what
  // std::round does on the CPU filter path and what WGSL's own round() does not: round() is
  // round-half-to-even, so it disagrees on every exact half. Running the emitted module on the
  // renderer's device and comparing every texel to std::round is what makes that a fact about
  // the shader rather than about a formula written twice, and it covers the vector forms of
  // sign and floor and both forms of pow at the same time.
  auto geodeDevice = donner::geode::GeodeDevice::CreateHeadless();
  if (!geodeDevice) {
    GTEST_SKIP() << "No WebGPU-capable device available";
  }

  ShaderResult<IrModule> module = BuildMathPrimitiveModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  ASSERT_FALSE(wgsl.hasError()) << "EmitWgsl failed: " << wgsl.error();

  const std::vector<float> values = MathPrimitiveInputValues();
  const std::vector<uint8_t> texels =
      RunMathPrimitiveModule(geodeDevice->device(), geodeDevice->queue(), wgsl.result(), values);
  ASSERT_THAT(texels, testing::SizeIs(values.size() * 4u));

  for (size_t i = 0; i < values.size(); ++i) {
    const float value = values[i];
    EXPECT_THAT(texels[i * 4u + 0u], testing::Eq(EncodeSignedResult(std::round(value))))
        << "rounding of " << value << " on the device diverges from std::round";
    EXPECT_THAT(texels[i * 4u + 1u], testing::Eq(EncodeSignResult(value))) << "sign of " << value;
    EXPECT_THAT(texels[i * 4u + 2u], testing::Eq(EncodeSignedResult(std::floor(value * 0.25f))))
        << "floor of " << value << " * 0.25";
    // pow is a transcendental with a per-implementation error bound, so its channel is held to
    // the quantization step rather than to an exact byte.
    EXPECT_NEAR(static_cast<double>(texels[i * 4u + 3u]),
                static_cast<double>(PowChannelOnHost(value)) * 255.0, 2.0)
        << "pow channel for " << value;
  }
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
