#pragma once
/// @file
/// \c donner::gpu::vulkan::VulkanResourceState - the tracked per-texture synchronization state
/// the Vulkan backend derives its barriers from.
///
/// Every barrier this backend records, and every render pass external dependency it declares,
/// is derived here from one table so the two cannot disagree. A precise image barrier sitting
/// behind a render pass dependency that still says ALL_COMMANDS buys nothing: the dependency is
/// the edge that orders an attachment write against whatever reads it next, so both halves read
/// from the same source of truth.
///
/// This is pure state machinery over Vulkan enums: it opens no device, calls no entry point, and
/// records nothing. That keeps it testable on every platform the project builds on, rather than
/// only where a Vulkan loader exists.
///
/// The tracked patterns are the ones the recorded command streams actually produce. Anything
/// outside them resolves to \ref ConservativeImageBarrier, which is the maximal
/// ALL_COMMANDS/memory barrier this backend used everywhere before: unknown usage costs
/// precision, never correctness.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <ostream>
#include <span>

#include "donner/gpu/Descriptors.h"

namespace donner::gpu::vulkan {

/// What a texture is being used for at one point of a recorded command stream.
///
/// Write-only storage textures are the only writable binding the runtime declares, so a storage
/// usage is unambiguously a write and needs no read/write fallback. If a writable storage BUFFER
/// binding kind ever enters the runtime, this enum and the table below must gain the
/// host-visibility edge that goes with it, because every buffer this backend allocates is
/// host-mapped and a kernel write to one would otherwise be invisible to the mapping.
enum class TextureUsageKind : uint8_t {
  Undefined,        //!< Never used; contents undefined.
  ColorAttachment,  //!< Written as a render pass color attachment.
  SampledRead,      //!< Read through a sampled-texture binding.
  StorageWrite,     //!< Written through a write-only storage-texture binding.
  TransferRead,     //!< Read as the source of a copy.
  TransferWrite,    //!< Written as the destination of a copy or an upload.
};

/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, TextureUsageKind value);

/// The synchronization state a texture is left in, as the backend's bookkeeping records it.
///
/// The stage and access describe what last *touched* the image, which is what a later barrier
/// names as its source scope. A freshly created texture has touched nothing, so it carries the
/// top-of-pipe stage and an empty access mask: a barrier out of that state waits for nothing,
/// which is correct rather than merely permissive.
struct TextureSyncState {
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;                //!< Current image layout.
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;  //!< Stage that last touched it.
  VkAccessFlags access = 0;                                        //!< Access that last touched it.

  /// Equality operator. @param other State to compare against.
  bool operator==(const TextureSyncState& other) const = default;
};

/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, const TextureSyncState& value);

/// The six values an image barrier is built from, plus whether precision was available.
struct ImageBarrierParams {
  VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  //!< Layout the image is in.
  VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;  //!< Layout the image moves to.
  VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;  //!< Source stage scope.
  VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;  //!< Destination scope.
  VkAccessFlags srcAccess = 0;                                         //!< Access made available.
  VkAccessFlags dstAccess = 0;                                         //!< Access made visible.
  /// True when the usage pair fell outside the tracked set and the maximal barrier was used.
  bool conservative = false;

  /// Equality operator. @param other Parameters to compare against.
  bool operator==(const ImageBarrierParams& other) const = default;
};

/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, const ImageBarrierParams& value);

/// The two halves of a render pass external subpass dependency.
struct SubpassDependencyParams {
  VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;  //!< Source stage scope.
  VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;  //!< Destination scope.
  VkAccessFlags srcAccess = 0;                                         //!< Access made available.
  VkAccessFlags dstAccess = 0;                                         //!< Access made visible.
  /// True when the declared usage implied nothing tracked and the maximal edge was used.
  bool conservative = false;

  /// Equality operator. @param other Parameters to compare against.
  bool operator==(const SubpassDependencyParams& other) const = default;
};

/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, const SubpassDependencyParams& value);

/// The maximal barrier: ALL_COMMANDS to ALL_COMMANDS with memory availability and visibility.
///
/// The fallback for any usage pair the table does not name. VK_ACCESS_MEMORY_* is valid with any
/// pipeline stage, so this is always legal, and it orders strictly more than any precise barrier
/// would - which is why an unrecognised pattern degrades to it rather than to nothing.
///
/// @param oldLayout Layout the image is in.
/// @param newLayout Layout the image moves to.
[[nodiscard]] ImageBarrierParams ConservativeImageBarrier(VkImageLayout oldLayout,
                                                          VkImageLayout newLayout);

/// The image layout a usage requires. @param usage Usage the texture is put to.
[[nodiscard]] VkImageLayout LayoutForUsage(TextureUsageKind usage);

/// The state a texture is left in once \p usage has executed against it.
/// @param usage Usage the texture is put to.
[[nodiscard]] TextureSyncState StateAfterUsage(TextureUsageKind usage);

/// The barrier that takes a texture from \p current to \p usage.
///
/// The source scope comes from what last touched the image and the destination scope from what
/// is about to, so the tracked patterns - attachment write to sampled read, storage write to
/// sampled read, transfer write to shader read, and their transfer counterparts - name the
/// stages and accesses actually involved instead of every stage and every access.
///
/// @param current State the texture is tracked in.
/// @param usage Usage the texture is about to be put to.
[[nodiscard]] ImageBarrierParams TransitionFor(const TextureSyncState& current,
                                               TextureUsageKind usage);

/// The external dependency into a render pass, ordering whatever touched the attachments before
/// it against the pass's color output.
///
/// One edge covers every attachment, so the source scope is the union across all of them: a pass
/// that loads one attachment a compute dispatch wrote and clears another must still wait for
/// that write, and taking any single attachment's prior state would lose it. A pass with no
/// attachments, or one whose attachment reached its layout through a path the model does not
/// describe, falls back to the maximal edge.
///
/// @param attachmentStates State each attachment texture is tracked in, in any order.
[[nodiscard]] SubpassDependencyParams AttachmentEntryDependency(
    std::span<const TextureSyncState> attachmentStates);

/// The external dependency out of a render pass, ordering the pass's color output against
/// whatever the attachment's declared usage says can consume it next.
///
/// The consumer is read from the texture's declared usage set rather than by scanning ahead: a
/// texture declared Sampled gets the shader-read edge, one declared CopySrc the transfer-read
/// edge, and one declaring several gets their union. A usage set naming no tracked consumer
/// falls back to the maximal edge.
///
/// @param declaredUsage Usage flags the attachment texture was created with.
[[nodiscard]] SubpassDependencyParams AttachmentExitDependency(TextureUsage declaredUsage);

/**
 * Tracked state for a set of textures, under the discipline the backend records against.
 *
 * A transition recorded into a command buffer updates the staged view; only a submission that
 * actually reached the queue commits it. An encode that fails partway, or a submit the driver
 * rejects, discards the staging instead - so the table never claims a transition the GPU never
 * ran, which would make every later barrier compute the wrong source scope from a state that
 * does not exist.
 */
class TextureSyncStateTable {
public:
  /// The state \p textureSlot will be in at this point of an encode: the staged state when one
  /// was recorded, otherwise the committed one. A slot never seen reads as untouched.
  /// @param textureSlot Texture slot to query.
  [[nodiscard]] TextureSyncState stateOf(uint32_t textureSlot) const;

  /// Records that a transition to \p state was encoded, pending submission.
  /// @param textureSlot Texture slot the transition applies to. @param state Resulting state.
  void stage(uint32_t textureSlot, const TextureSyncState& state);

  /// Promotes every staged transition to committed. Call only once the submission succeeded.
  void commitStaged();

  /// Drops every staged transition, leaving committed state untouched.
  void discardStaged();

  /// Forgets a slot entirely, for a texture that no longer exists.
  /// @param textureSlot Texture slot to drop.
  void forget(uint32_t textureSlot);

  /// Whether any transition is staged and not yet committed.
  [[nodiscard]] bool hasStagedChanges() const;

private:
  std::map<uint32_t, TextureSyncState> committed_;  //!< State the GPU has actually reached.
  std::map<uint32_t, TextureSyncState> staged_;     //!< Transitions encoded but not submitted.
};

}  // namespace donner::gpu::vulkan
