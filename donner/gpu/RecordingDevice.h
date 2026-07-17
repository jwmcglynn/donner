#pragma once
/// @file
/// \c donner::gpu::RecordingDevice - the deterministic recording backend.

#include <string>
#include <vector>

#include "donner/gpu/Device.h"

namespace donner::gpu {

/**
 * Deterministic recording backend (design 0053 "Proposed Architecture", "Testing Strategy").
 *
 * Inherits every fail-closed validation check from \ref Device and records each validated
 * operation as a line of text. \ref serialize returns the capture: repeated identical operation
 * streams serialize byte-identically, resources are identified by stable slot-based ids like
 * `buffer#0` (never pointers or process state), descriptor fields are dumped in full with fixed
 * formatting, and data payloads appear as byte counts plus a content hash.
 *
 * Submission completes instantly: \ref completedSerial always equals
 * \ref Device::lastSubmittedSerial.
 */
class RecordingDevice final : public Device {
public:
  /// Constructs an empty recording device.
  RecordingDevice() = default;

  /// Destructor.
  ~RecordingDevice() override = default;

  /**
   * Returns the deterministic line-based text capture of every recorded operation, in order.
   * Creation, write, and destroy operations appear as one line each; submissions appear as a
   * `submit` line followed by one indented line per command.
   */
  std::string serialize() const;

  /// Serial of the most recent submission; recording completes instantly.
  uint64_t completedSerial() const override { return lastSubmittedSerial(); }

protected:
  Status onCreateBuffer(uint32_t slotIndex, const BufferDescriptor& descriptor) override;
  Status onCreateTexture(uint32_t slotIndex, const TextureDescriptor& descriptor) override;
  Status onCreateTextureView(uint32_t slotIndex, uint32_t textureSlotIndex,
                             const TextureViewDescriptor& descriptor) override;
  Status onCreateSampler(uint32_t slotIndex, const SamplerDescriptor& descriptor) override;
  Status onCreateBindGroupLayout(uint32_t slotIndex,
                                 const BindGroupLayoutDescriptor& descriptor) override;
  Status onCreateBindGroup(uint32_t slotIndex, const BindGroupDescriptor& descriptor) override;
  Status onCreatePipelineLayout(uint32_t slotIndex,
                                const PipelineLayoutDescriptor& descriptor) override;
  Status onCreateShaderModule(uint32_t slotIndex,
                              const ShaderModuleDescriptor& descriptor) override;
  Status onCreateRenderPipeline(uint32_t slotIndex,
                                const RenderPipelineDescriptor& descriptor) override;
  void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) override;
  Status onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                       std::span<const uint8_t> data) override;
  Status onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                        const TexelCopyBufferLayout& dataLayout,
                        const Extent2d& writeSize) override;
  Status onSubmit(uint64_t submissionSerial, uint32_t commandBufferSlotIndex,
                  std::span<const Command> commands) override;

private:
  std::vector<std::string> lines_;
};

}  // namespace donner::gpu
