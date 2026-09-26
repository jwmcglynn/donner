#pragma once
/// @file
/// One Geode fill workload shared by the structural gate and the nightly CPU benchmark.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/DeviceObserver.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"

namespace donner::geode::benchmarks {

/// CPU time for one accepted submission. Pipeline/resource creation and GPU completion are outside
/// both timed intervals.
struct DrawCpuSample {
  uint32_t draws = 0;         //!< Draws submitted.
  uint64_t recordingNs = 0;   //!< Encoder creation through finish, in nanoseconds.
  uint64_t submissionNs = 0;  //!< Device::submit, including backend encoding, in nanoseconds.
};

/// A fixed observer avoids allocating in the measured Device::submit path.
class DrawCountObserver final : public gpu::DeviceObserver {
public:
  void onBufferCreated() override {}
  void onTextureCreated() override {}
  void onTextureReleased() override {}
  void onBindGroupCreated() override {}
  void onBufferWritten(uint64_t) override {}
  void onTextureWritten(uint64_t) override {}
  void onSubmitted(uint64_t commandBufferCount, uint64_t drawCount) override {
    if (commandBufferCount == 1) {
      ++submissions;
      lastDraws = drawCount;
    }
  }

  uint64_t submissions = 0;  //!< Observed single-command-buffer submissions.
  uint64_t lastDraws = 0;    //!< Draw count in the last such submission.
};

/// Uses the shipped SlugFill pipeline and all eleven reflected bind slots. One target and bind
/// group serve every sample so the timer isolates command recording and submission.
class DrawCpuWorkload {
public:
  /// @param device Backend to measure; must outlive this workload.
  explicit DrawCpuWorkload(gpu::Device& device) : device_(device) {}
  DrawCpuWorkload(const DrawCpuWorkload&) = delete;
  DrawCpuWorkload& operator=(const DrawCpuWorkload&) = delete;
  ~DrawCpuWorkload() {
    if (observerInstalled_) {
      device_.removeObserver(observer_);
    }
  }

  /// Creates and uploads a small, valid fill scene before timing starts.
  bool initialize() {
    pipeline_ = std::make_unique<GeodePipeline>(device_, gpu::TextureFormat::RGBA8Unorm);
    if (!take(device_.createTexture(gpu::TextureDescriptor{"drawCpuTarget",
                                                           {16, 16},
                                                           gpu::TextureFormat::RGBA8Unorm,
                                                           gpu::TextureUsage::RenderAttachment}),
              target_) ||
        !take(device_.createTextureView(target_, {"drawCpuTargetView"}), targetView_) ||
        !take(device_.createTexture(
                  gpu::TextureDescriptor{"drawCpuSampled",
                                         {1, 1},
                                         gpu::TextureFormat::RGBA8Unorm,
                                         gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst}),
              sampled_) ||
        !take(device_.createTextureView(sampled_, {"drawCpuSampledView"}), sampledView_) ||
        !take(device_.createSampler(gpu::SamplerDescriptor{
                  "drawCpuSampler", gpu::FilterMode::Linear, gpu::FilterMode::Linear,
                  gpu::AddressMode::Repeat, gpu::AddressMode::Repeat}),
              sampler_)) {
      return false;
    }

    const gpu::shader::CompiledShaderView& shader = gpu::shader::programs::SlugFillShader();
    if (shader.resources.size() != 11) {
      ADD_FAILURE() << "SlugFill resource count drifted: " << shader.resources.size();
      return false;
    }
    uint64_t storageBytes = 1024;
    for (const gpu::shader::ShaderResource& resource : shader.resources) {
      if (resource.type == gpu::BindingType::ReadOnlyStorageBuffer) {
        storageBytes = std::max(storageBytes, static_cast<uint64_t>(resource.minSizeBytes));
      }
    }
    if (!take(device_.createBuffer(gpu::BufferDescriptor{
                  "drawCpuUniform", sizeof(gpu::shader::programs::SlugFillParams),
                  gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst}),
              uniform_) ||
        !take(device_.createBuffer(
                  gpu::BufferDescriptor{"drawCpuStorage", storageBytes,
                                        gpu::BufferUsage::Storage | gpu::BufferUsage::CopyDst}),
              storage_)) {
      return false;
    }

    gpu::shader::programs::SlugFillParams params{};
    params.mvp[0] = params.mvp[5] = params.mvp[10] = params.mvp[15] = 1.0f;
    params.patternFromPath[0] = params.patternFromPath[5] = 1.0f;
    params.patternFromPath[10] = params.patternFromPath[15] = 1.0f;
    params.viewport[0] = params.viewport[1] = 16.0f;
    params.color[0] = params.color[3] = 1.0f;
    params.boundingVertexCount = 3;
    params.boundingVertices[0] = -0.5f;
    params.boundingVertices[1] = -0.5f;
    params.boundingVertices[2] = 0.5f;
    params.boundingVertices[3] = -0.5f;
    params.boundingVertices[4] = 0.0f;
    params.boundingVertices[5] = 0.5f;
    params.gridHStride = params.gridVStride = 1.0f;
    params.pathFromPixel[0] = params.pathFromPixel[3] = 1.0f;
    const std::span<const uint8_t> uniformData(reinterpret_cast<const uint8_t*>(&params),
                                               sizeof(params));
    const std::vector<uint8_t> zeroStorage(storageBytes, 0);
    std::array<uint8_t, gpu::kTexelRowPitchAlignment> texelRow{};
    texelRow[3] = 255;
    if (!accept(device_.writeBuffer(uniform_, 0, uniformData)) ||
        !accept(device_.writeBuffer(storage_, 0, zeroStorage)) ||
        !accept(device_.writeTexture(sampled_, texelRow,
                                     gpu::TexelCopyBufferLayout{0, gpu::kTexelRowPitchAlignment, 1},
                                     gpu::Extent2d{1, 1}))) {
      return false;
    }

    std::vector<gpu::BindGroupEntry> entries;
    entries.reserve(shader.resources.size());
    for (const gpu::shader::ShaderResource& resource : shader.resources) {
      switch (resource.type) {
        case gpu::BindingType::UniformBuffer:
          entries.push_back({resource.binding, gpu::BufferBinding{uniform_, 0, sizeof(params)}});
          break;
        case gpu::BindingType::ReadOnlyStorageBuffer:
          entries.push_back({resource.binding, gpu::BufferBinding{storage_, 0, storageBytes}});
          break;
        case gpu::BindingType::SampledTexture2dFloat:
        case gpu::BindingType::SampledTexture2dUnfilterableFloat:
          entries.push_back({resource.binding, gpu::TextureViewBinding{sampledView_}});
          break;
        case gpu::BindingType::FilteringSampler:
          entries.push_back({resource.binding, gpu::SamplerBinding{sampler_}});
          break;
        default:
          ADD_FAILURE() << "Unsupported SlugFill binding " << resource.name.view();
          return false;
      }
    }
    if (!take(device_.createBindGroup(gpu::BindGroupDescriptor{
                  "drawCpuBindGroup", pipeline_->bindGroupLayout(), std::move(entries)}),
              group_) ||
        !accept(device_.installObserver(observer_))) {
      return false;
    }
    observerInstalled_ = true;
    return true;
  }

  /// Records and submits N real triangle draws; checks the observer after each sample.
  /// @param draws Number of draws, greater than zero.
  /// @return Timings, or no value after an explicit failure.
  std::optional<DrawCpuSample> run(uint32_t draws) {
    if (draws == 0 || !observerInstalled_) {
      ADD_FAILURE() << "Draw workload must be initialized and nonempty";
      return std::nullopt;
    }
    const uint64_t submissionsBefore = observer_.submissions;
    const auto recordStart = std::chrono::steady_clock::now();
    gpu::Result<std::unique_ptr<gpu::CommandEncoder>> encoderResult =
        device_.createCommandEncoder();
    if (encoderResult.hasError()) {
      ADD_FAILURE() << encoderResult.error();
      return std::nullopt;
    }
    std::unique_ptr<gpu::CommandEncoder> encoder = std::move(encoderResult).result();
    gpu::Result<gpu::RenderPassEncoder*> passResult = encoder->beginRenderPass(
        gpu::RenderPassDescriptor{"drawCpuPass",
                                  {gpu::RenderPassColorAttachment{
                                      targetView_, gpu::LoadOp::Clear, gpu::StoreOp::Store, {}}}});
    if (passResult.hasError()) {
      ADD_FAILURE() << passResult.error();
      return std::nullopt;
    }
    gpu::RenderPassEncoder* pass = passResult.result();
    if (!accept(pass->setPipeline(pipeline_->pipeline())) ||
        !accept(pass->setBindGroup(0, group_))) {
      return std::nullopt;
    }
    for (uint32_t index = 0; index < draws; ++index) {
      if (!accept(pass->draw(3))) {
        return std::nullopt;
      }
    }
    if (!accept(pass->end())) {
      return std::nullopt;
    }
    gpu::Result<gpu::CommandBuffer> commandResult = encoder->finish();
    if (commandResult.hasError()) {
      ADD_FAILURE() << commandResult.error();
      return std::nullopt;
    }
    const auto recordEnd = std::chrono::steady_clock::now();
    const auto submitStart = std::chrono::steady_clock::now();
    gpu::Result<uint64_t> submission = device_.submit(std::move(commandResult).result());
    const auto submitEnd = std::chrono::steady_clock::now();
    if (submission.hasError()) {
      ADD_FAILURE() << submission.error();
      return std::nullopt;
    }
    if (observer_.submissions != submissionsBefore + 1 || observer_.lastDraws != draws) {
      ADD_FAILURE() << "onSubmitted expected (1," << draws << ") once; observed "
                    << (observer_.submissions - submissionsBefore) << " calls, last draw count "
                    << observer_.lastDraws;
      return std::nullopt;
    }
    const uint64_t serial = submission.result();
    if (!device_.waitForSerial(serial, 10.0)) {
      ADD_FAILURE() << "GPU submission " << serial << " did not complete";
      return std::nullopt;
    }
    return DrawCpuSample{
        draws,
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(recordEnd - recordStart).count()),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(submitEnd - submitStart).count())};
  }

private:
  template <typename T>
  static bool take(gpu::Result<T>&& result, T& output) {
    if (result.hasError()) {
      ADD_FAILURE() << result.error();
      return false;
    }
    output = std::move(result).result();
    return true;
  }

  static bool accept(gpu::Status status) {
    if (status.hasError()) {
      ADD_FAILURE() << status.error();
      return false;
    }
    return true;
  }

  gpu::Device& device_;
  std::unique_ptr<GeodePipeline> pipeline_;
  gpu::Texture target_;
  gpu::TextureView targetView_;
  gpu::Texture sampled_;
  gpu::TextureView sampledView_;
  gpu::Sampler sampler_;
  gpu::Buffer uniform_;
  gpu::Buffer storage_;
  gpu::BindGroup group_;
  DrawCountObserver observer_;
  bool observerInstalled_ = false;
};

}  // namespace donner::geode::benchmarks
