#include "donner/editor/gui/UiTextureRegistry.h"

#include <format>
#include <string>
#include <utility>

#include "donner/gpu/Device.h"

namespace donner::editor {

namespace {

/// Formats an identifier the way \ref PrintTo does, for use inside error messages.
/// @param id Identifier to format.
std::string DescribeId(UiTextureId id) {
  if (!id.isValid()) {
    return "uiTexture(null)";
  }
  return std::format("uiTexture#{}@{}", id.slotIndex(), id.generation());
}

}  // namespace

std::ostream& operator<<(std::ostream& os, UiTextureAlphaMode value) {
  switch (value) {
    case UiTextureAlphaMode::Premultiplied: return os << "Premultiplied";
    case UiTextureAlphaMode::Straight: return os << "Straight";
  }

  return os << "Unknown";
}

void PrintTo(const UiTextureId& id, std::ostream* os) {
  *os << DescribeId(id);
}

void PrintTo(const UiTextureBinding& binding, std::ostream* os) {
  *os << "UiTextureBinding { view: ";
  gpu::PrintTo(binding.view, os);
  *os << ", size: " << binding.size.width << "x" << binding.size.height
      << ", alphaMode: " << binding.alphaMode << " }";
}

UiTextureRegistry::UiTextureRegistry(const gpu::Device& device, uint32_t retirementFrames)
    : device_(&device), retirementFrames_(retirementFrames) {}

UiTextureRegistry::~UiTextureRegistry() = default;

gpu::Result<UiTextureId> UiTextureRegistry::registerTexture(const UiTextureDescriptor& descriptor) {
  if (!descriptor.view.isValid()) {
    return gpu::GpuError{gpu::GpuErrorType::InvalidHandle,
                         "UI texture view is null (default-constructed or moved-from)"};
  }
  if (descriptor.view.deviceId() != device_->deviceId()) {
    return gpu::GpuError{
        gpu::GpuErrorType::DeviceMismatch,
        std::format("UI texture view belongs to device {} but was registered with device {}",
                    descriptor.view.deviceId(), device_->deviceId())};
  }
  if (descriptor.size.width == 0 || descriptor.size.height == 0) {
    return gpu::GpuError{gpu::GpuErrorType::InvalidDescriptor,
                         std::format("UI texture size must be nonzero, was {}x{}",
                                     descriptor.size.width, descriptor.size.height)};
  }

  const UiTextureBinding binding{descriptor.view, descriptor.size, descriptor.alphaMode};

  if (!freeSlots_.empty()) {
    const uint32_t slotIndex = freeSlots_.back();
    freeSlots_.pop_back();
    Slot& slot = slots_[slotIndex];
    ++slot.generation;
    slot.alive = true;
    slot.retired = false;
    slot.framesSinceRetired = 0;
    slot.binding = binding;
    return UiTextureId::CreateForRegistry(slotIndex, slot.generation);
  }

  if (slots_.size() >= kMaxRegistrations) {
    return gpu::GpuError{
        gpu::GpuErrorType::LimitExceeded,
        std::format("UI texture registry holds {} registrations, at its {} registration bound",
                    slots_.size(), kMaxRegistrations)};
  }

  slots_.push_back(Slot{.generation = 1,
                        .alive = true,
                        .retired = false,
                        .framesSinceRetired = 0,
                        .binding = binding});
  return UiTextureId::CreateForRegistry(static_cast<uint32_t>(slots_.size() - 1), 1);
}

gpu::Result<const UiTextureRegistry::Slot*> UiTextureRegistry::resolveSlot(
    UiTextureId id, std::string_view operation) const {
  if (!id.isValid()) {
    return gpu::GpuError{
        gpu::GpuErrorType::InvalidHandle,
        std::format("UI texture id is null (default-constructed) in {}", operation)};
  }
  if (id.slotIndex() >= slots_.size()) {
    return gpu::GpuError{gpu::GpuErrorType::InvalidHandle,
                         std::format("UI texture id {} names a slot this registry never handed "
                                     "out (registry holds {} slots) in {}",
                                     DescribeId(id), slots_.size(), operation)};
  }

  const Slot& slot = slots_[id.slotIndex()];
  if (!slot.alive || slot.generation != id.generation()) {
    return gpu::GpuError{gpu::GpuErrorType::InvalidHandle,
                         std::format("UI texture id {} is stale; its slot now holds generation "
                                     "{} in {}",
                                     DescribeId(id), slot.generation, operation)};
  }
  if (slot.retired) {
    return gpu::GpuError{gpu::GpuErrorType::InvalidState,
                         std::format("UI texture id {} was retired {} frames ago in {}",
                                     DescribeId(id), slot.framesSinceRetired, operation)};
  }
  if (slot.binding.view.deviceId() != device_->deviceId()) {
    return gpu::GpuError{
        gpu::GpuErrorType::DeviceMismatch,
        std::format("UI texture id {} references device {} but this registry serves device {} "
                    "in {}",
                    DescribeId(id), slot.binding.view.deviceId(), device_->deviceId(), operation)};
  }
  return &slot;
}

gpu::Result<UiTextureBinding> UiTextureRegistry::lookup(UiTextureId id) const {
  gpu::Result<const Slot*> slot = resolveSlot(id, "lookup");
  if (slot.hasError()) {
    return std::move(slot).error();
  }
  return slot.result()->binding;
}

gpu::Status UiTextureRegistry::retire(UiTextureId id) {
  gpu::Result<const Slot*> slot = resolveSlot(id, "retire");
  if (slot.hasError()) {
    return std::move(slot).error();
  }

  Slot& mutableSlot = slots_[id.slotIndex()];
  mutableSlot.retired = true;
  mutableSlot.framesSinceRetired = 0;
  return gpu::OkStatus();
}

std::vector<UiTextureId> UiTextureRegistry::advanceFrame() {
  std::vector<UiTextureId> released;
  for (uint32_t slotIndex = 0; slotIndex < slots_.size(); ++slotIndex) {
    Slot& slot = slots_[slotIndex];
    if (!slot.alive || !slot.retired) {
      continue;
    }

    ++slot.framesSinceRetired;
    if (slot.framesSinceRetired < retirementFrames_) {
      continue;
    }

    released.push_back(UiTextureId::CreateForRegistry(slotIndex, slot.generation));
    slot.alive = false;
    slot.retired = false;
    slot.framesSinceRetired = 0;
    slot.binding = UiTextureBinding{};
    freeSlots_.push_back(slotIndex);
  }
  return released;
}

size_t UiTextureRegistry::liveCount() const {
  size_t count = 0;
  for (const Slot& slot : slots_) {
    if (slot.alive && !slot.retired) {
      ++count;
    }
  }
  return count;
}

size_t UiTextureRegistry::retiredCount() const {
  size_t count = 0;
  for (const Slot& slot : slots_) {
    if (slot.alive && slot.retired) {
      ++count;
    }
  }
  return count;
}

}  // namespace donner::editor
