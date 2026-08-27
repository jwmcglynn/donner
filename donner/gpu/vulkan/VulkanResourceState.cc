/// @file
/// Implementation of the Vulkan backend's tracked resource-state model.

#include "donner/gpu/vulkan/VulkanResourceState.h"

namespace donner::gpu::vulkan {

namespace {

/// The stages a sampled read can happen in. A sampled texture is readable from the fragment
/// stage of a render pass and from a compute dispatch, and the backend does not record which of
/// the two a given binding will be read from, so the destination scope names both.
constexpr VkPipelineStageFlags kSampledReadStages =
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

/// The access bits that need an availability operation. Only writes have to be made available;
/// a read that happened before needs no flushing, so naming it in a source scope would order
/// strictly more than the hazard requires.
constexpr VkAccessFlags kWriteAccessMask =
    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

/// Whether this component is the one that put a texture in \p layout. A layout it never produces
/// came from somewhere it does not model, so a transition out of it cannot claim to know what
/// last touched the image. @param layout Layout to classify.
bool IsTrackedLayout(VkImageLayout layout) {
  switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
    case VK_IMAGE_LAYOUT_GENERAL:
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return true;
    default: break;
  }
  return false;
}

}  // namespace

std::ostream& operator<<(std::ostream& os, TextureUsageKind value) {
  switch (value) {
    case TextureUsageKind::Undefined: return os << "Undefined";
    case TextureUsageKind::ColorAttachment: return os << "ColorAttachment";
    case TextureUsageKind::SampledRead: return os << "SampledRead";
    case TextureUsageKind::StorageWrite: return os << "StorageWrite";
    case TextureUsageKind::TransferRead: return os << "TransferRead";
    case TextureUsageKind::TransferWrite: return os << "TransferWrite";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, const TextureSyncState& value) {
  return os << "{layout=" << static_cast<int>(value.layout) << ", stage=0x" << std::hex
            << value.stage << ", access=0x" << value.access << std::dec << "}";
}

std::ostream& operator<<(std::ostream& os, const ImageBarrierParams& value) {
  return os << "{oldLayout=" << static_cast<int>(value.oldLayout)
            << ", newLayout=" << static_cast<int>(value.newLayout) << ", srcStage=0x" << std::hex
            << value.srcStage << ", dstStage=0x" << value.dstStage << ", srcAccess=0x"
            << value.srcAccess << ", dstAccess=0x" << value.dstAccess << std::dec
            << ", conservative=" << (value.conservative ? "true" : "false") << "}";
}

std::ostream& operator<<(std::ostream& os, const SubpassDependencyParams& value) {
  return os << "{srcStage=0x" << std::hex << value.srcStage << ", dstStage=0x" << value.dstStage
            << ", srcAccess=0x" << value.srcAccess << ", dstAccess=0x" << value.dstAccess
            << std::dec << ", conservative=" << (value.conservative ? "true" : "false") << "}";
}

namespace {

/// The one maximal scope every conservative fallback uses: wait for everything, make every write
/// available, make every access visible. VK_ACCESS_MEMORY_* is valid with any stage, so this is
/// always legal and always orders at least as much as a precise scope would.
namespace maximal {
constexpr VkPipelineStageFlags kStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
constexpr VkAccessFlags kSrcAccess = VK_ACCESS_MEMORY_WRITE_BIT;
constexpr VkAccessFlags kDstAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
}  // namespace maximal

/// The maximal subpass dependency, for a pass whose scope cannot be narrowed.
SubpassDependencyParams ConservativeSubpassDependency() {
  SubpassDependencyParams params;
  params.srcStage = maximal::kStage;
  params.dstStage = maximal::kStage;
  params.srcAccess = maximal::kSrcAccess;
  params.dstAccess = maximal::kDstAccess;
  params.conservative = true;
  return params;
}

}  // namespace

ImageBarrierParams ConservativeImageBarrier(VkImageLayout oldLayout, VkImageLayout newLayout) {
  ImageBarrierParams params;
  params.oldLayout = oldLayout;
  params.newLayout = newLayout;
  params.srcStage = maximal::kStage;
  params.dstStage = maximal::kStage;
  params.srcAccess = maximal::kSrcAccess;
  params.dstAccess = maximal::kDstAccess;
  params.conservative = true;
  return params;
}

VkImageLayout LayoutForUsage(TextureUsageKind usage) {
  switch (usage) {
    case TextureUsageKind::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
    case TextureUsageKind::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case TextureUsageKind::SampledRead: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case TextureUsageKind::StorageWrite: return VK_IMAGE_LAYOUT_GENERAL;
    case TextureUsageKind::TransferRead: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case TextureUsageKind::TransferWrite: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  }
  return VK_IMAGE_LAYOUT_UNDEFINED;
}

TextureSyncState StateAfterUsage(TextureUsageKind usage) {
  switch (usage) {
    case TextureUsageKind::Undefined:
      // Nothing has touched the image, so a barrier out of this state waits for nothing.
      return TextureSyncState{VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
    case TextureUsageKind::ColorAttachment:
      return TextureSyncState{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case TextureUsageKind::SampledRead:
      return TextureSyncState{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kSampledReadStages,
                              VK_ACCESS_SHADER_READ_BIT};
    case TextureUsageKind::StorageWrite:
      return TextureSyncState{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_ACCESS_SHADER_WRITE_BIT};
    case TextureUsageKind::TransferRead:
      return TextureSyncState{VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_READ_BIT};
    case TextureUsageKind::TransferWrite:
      return TextureSyncState{VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT};
  }
  return TextureSyncState{};
}

ImageBarrierParams TransitionFor(const TextureSyncState& current, TextureUsageKind usage) {
  const VkImageLayout newLayout = LayoutForUsage(usage);
  if (!IsTrackedLayout(current.layout) || usage == TextureUsageKind::Undefined) {
    // Either the image reached its layout through a path this table does not model, or the
    // caller named no real destination usage. Neither leaves anything precise to say.
    return ConservativeImageBarrier(current.layout, newLayout);
  }

  const TextureSyncState next = StateAfterUsage(usage);
  ImageBarrierParams params;
  params.oldLayout = current.layout;
  params.newLayout = newLayout;
  params.srcStage = current.stage;
  params.dstStage = next.stage;
  params.srcAccess = current.access & kWriteAccessMask;
  params.dstAccess = next.access;
  params.conservative = false;
  return params;
}

SubpassDependencyParams AttachmentEntryDependency(
    std::span<const TextureSyncState> attachmentStates) {
  if (attachmentStates.empty()) {
    return ConservativeSubpassDependency();
  }

  SubpassDependencyParams params;
  params.srcStage = 0;
  params.srcAccess = 0;
  for (const TextureSyncState& state : attachmentStates) {
    if (!IsTrackedLayout(state.layout)) {
      return ConservativeSubpassDependency();
    }
    // One edge serves every attachment, so it must wait for the widest thing any of them was
    // last touched by. Narrowing to a single attachment would drop another's prior write: a pass
    // that loads one attachment a dispatch wrote and clears another still has to wait for that
    // write to be both available and visible to the load.
    params.srcStage |= state.stage;
    params.srcAccess |= state.access & kWriteAccessMask;
  }
  params.dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  params.dstAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  params.conservative = false;
  return params;
}

SubpassDependencyParams AttachmentExitDependency(TextureUsage declaredUsage) {
  VkPipelineStageFlags dstStage = 0;
  VkAccessFlags dstAccess = 0;
  if (HasAllFlags(declaredUsage, TextureUsage::Sampled)) {
    dstStage |= kSampledReadStages;
    dstAccess |= VK_ACCESS_SHADER_READ_BIT;
  }
  if (HasAllFlags(declaredUsage, TextureUsage::StorageBinding)) {
    dstStage |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dstAccess |= VK_ACCESS_SHADER_WRITE_BIT;
  }
  if (HasAllFlags(declaredUsage, TextureUsage::CopySrc)) {
    dstStage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    dstAccess |= VK_ACCESS_TRANSFER_READ_BIT;
  }
  if (HasAllFlags(declaredUsage, TextureUsage::CopyDst)) {
    dstStage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    dstAccess |= VK_ACCESS_TRANSFER_WRITE_BIT;
  }
  if (HasAllFlags(declaredUsage, TextureUsage::RenderAttachment)) {
    dstStage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dstAccess |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  }

  if (dstStage == 0) {
    // The attachment declares no consumer this table models, so nothing narrower than the
    // maximal edge can be justified.
    return ConservativeSubpassDependency();
  }
  SubpassDependencyParams params;
  params.srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  params.srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  params.dstStage = dstStage;
  params.dstAccess = dstAccess;
  params.conservative = false;
  return params;
}

TextureSyncState TextureSyncStateTable::stateOf(uint32_t textureSlot) const {
  if (const auto staged = staged_.find(textureSlot); staged != staged_.end()) {
    return staged->second;
  }
  if (const auto committed = committed_.find(textureSlot); committed != committed_.end()) {
    return committed->second;
  }
  return TextureSyncState{};
}

void TextureSyncStateTable::stage(uint32_t textureSlot, const TextureSyncState& state) {
  staged_[textureSlot] = state;
}

void TextureSyncStateTable::commitStaged() {
  for (const auto& [textureSlot, state] : staged_) {
    committed_[textureSlot] = state;
  }
  staged_.clear();
}

void TextureSyncStateTable::discardStaged() {
  staged_.clear();
}

void TextureSyncStateTable::forget(uint32_t textureSlot) {
  committed_.erase(textureSlot);
  staged_.erase(textureSlot);
}

bool TextureSyncStateTable::hasStagedChanges() const {
  return !staged_.empty();
}

}  // namespace donner::gpu::vulkan
