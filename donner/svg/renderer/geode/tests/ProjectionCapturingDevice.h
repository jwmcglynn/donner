#pragma once
/// @file
/// A device that reports a chosen shader source kind and keeps the shader module descriptor it last
/// accepted, so a test can assert which projection a production creator selected.

#include <cstdint>
#include <span>
#include <string_view>

#include "donner/gpu/Device.h"

namespace donner::geode::tests {

/**
 * Device reporting a caller-chosen shader source kind, keeping the descriptor of the shader
 * module it last accepted so a test can assert which projection a creator selected.
 *
 * Inherits every fail-closed check from \ref gpu::Device; the remaining backend operations
 * succeed without recording anything.
 */
class ProjectionCapturingDevice final : public gpu::Device {
public:
  /// @param kind Source kind this device reports to callers.
  explicit ProjectionCapturingDevice(gpu::ShaderSourceKind kind) : kind_(kind) {}

  gpu::ShaderSourceKind shaderSourceKind() const override { return kind_; }
  uint64_t completedSerial() const override { return lastSubmittedSerial(); }

  /// Descriptor of the most recently accepted shader module; default-constructed until one is.
  const gpu::ShaderModuleDescriptor& lastDescriptor() const { return lastDescriptor_; }

protected:
  gpu::Status onCreateShaderModule(uint32_t,
                                   const gpu::ShaderModuleDescriptor& descriptor) override {
    lastDescriptor_ = descriptor;
    return gpu::OkStatus();
  }

  gpu::Status onCreateBuffer(uint32_t, const gpu::BufferDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateTexture(uint32_t, const gpu::TextureDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateTextureView(uint32_t, uint32_t, const gpu::TextureViewDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateSampler(uint32_t, const gpu::SamplerDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateBindGroupLayout(uint32_t, const gpu::BindGroupLayoutDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateBindGroup(uint32_t, const gpu::BindGroupDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreatePipelineLayout(uint32_t, const gpu::PipelineLayoutDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateRenderPipeline(uint32_t, const gpu::RenderPipelineDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateComputePipeline(uint32_t, const gpu::ComputePipelineDescriptor&) override {
    return gpu::OkStatus();
  }
  void onDestroyResource(std::string_view, uint32_t) override {}
  gpu::Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override {
    return gpu::OkStatus();
  }
  gpu::Status onWriteTexture(uint32_t, std::span<const uint8_t>, const gpu::TexelCopyBufferLayout&,
                             const gpu::Extent2d&, const gpu::Origin2d&) override {
    return gpu::OkStatus();
  }
  gpu::Status onSubmit(uint64_t, std::span<const gpu::SubmittedCommandBuffer>) override {
    return gpu::OkStatus();
  }

private:
  gpu::ShaderSourceKind kind_;
  gpu::ShaderModuleDescriptor lastDescriptor_;
};

}  // namespace donner::geode::tests
