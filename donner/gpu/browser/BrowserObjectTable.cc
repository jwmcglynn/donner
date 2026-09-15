#include "donner/gpu/browser/BrowserObjectTable.h"

#include <limits>

namespace donner::gpu::browser {

std::string_view BrowserObjectKindName(BrowserObjectKind kind) {
  switch (kind) {
    case BrowserObjectKind::Buffer: return "buffer";
    case BrowserObjectKind::Texture: return "texture";
    case BrowserObjectKind::TextureView: return "texture view";
    case BrowserObjectKind::Sampler: return "sampler";
    case BrowserObjectKind::BindGroupLayout: return "bind group layout";
    case BrowserObjectKind::BindGroup: return "bind group";
    case BrowserObjectKind::PipelineLayout: return "pipeline layout";
    case BrowserObjectKind::ShaderModule: return "shader module";
    case BrowserObjectKind::RenderPipeline: return "render pipeline";
    case BrowserObjectKind::ComputePipeline: return "compute pipeline";
    case BrowserObjectKind::Surface: return "surface";
    case BrowserObjectKind::BufferMapping: return "buffer mapping";
    case BrowserObjectKind::kCount: break;
  }
  return "unknown object";
}

std::ostream& operator<<(std::ostream& os, BrowserObjectKind value) {
  return os << BrowserObjectKindName(value);
}

BrowserObjectInsertion BrowserObjectTable::insert(BrowserObjectKind kind, uint32_t slotIndex) {
  const size_t kindIndex = static_cast<size_t>(kind);
  if (kindIndex >= kBrowserObjectKindCount || slotIndex > kMaxSlotIndex) {
    return BrowserObjectInsertion{};
  }
  if (nextId_ == std::numeric_limits<BrowserObjectId>::max()) {
    return BrowserObjectInsertion{};
  }

  std::vector<BrowserObjectId>& slots = slotsByKind_[kindIndex];
  if (slotIndex >= slots.size()) {
    slots.resize(static_cast<size_t>(slotIndex) + 1, kNoBrowserObject);
  }

  BrowserObjectInsertion insertion;
  insertion.displaced = slots[slotIndex];
  insertion.id = nextId_++;
  slots[slotIndex] = insertion.id;
  return insertion;
}

std::optional<BrowserObjectId> BrowserObjectTable::find(BrowserObjectKind kind,
                                                        uint32_t slotIndex) const {
  const size_t kindIndex = static_cast<size_t>(kind);
  if (kindIndex >= kBrowserObjectKindCount) {
    return std::nullopt;
  }
  const std::vector<BrowserObjectId>& slots = slotsByKind_[kindIndex];
  if (slotIndex >= slots.size() || slots[slotIndex] == kNoBrowserObject) {
    return std::nullopt;
  }
  return slots[slotIndex];
}

std::optional<BrowserObjectId> BrowserObjectTable::remove(BrowserObjectKind kind,
                                                          uint32_t slotIndex) {
  const std::optional<BrowserObjectId> id = find(kind, slotIndex);
  if (id.has_value()) {
    slotsByKind_[static_cast<size_t>(kind)][slotIndex] = kNoBrowserObject;
  }
  return id;
}

std::vector<std::pair<BrowserObjectKind, BrowserObjectId>> BrowserObjectTable::takeAll() {
  std::vector<std::pair<BrowserObjectKind, BrowserObjectId>> taken;
  taken.reserve(liveCount());
  for (size_t kindIndex = 0; kindIndex < kBrowserObjectKindCount; ++kindIndex) {
    std::vector<BrowserObjectId>& slots = slotsByKind_[kindIndex];
    for (BrowserObjectId& id : slots) {
      if (id != kNoBrowserObject) {
        taken.emplace_back(static_cast<BrowserObjectKind>(kindIndex), id);
        id = kNoBrowserObject;
      }
    }
  }
  return taken;
}

size_t BrowserObjectTable::liveCount() const {
  size_t count = 0;
  for (const std::vector<BrowserObjectId>& slots : slotsByKind_) {
    for (const BrowserObjectId id : slots) {
      if (id != kNoBrowserObject) {
        ++count;
      }
    }
  }
  return count;
}

}  // namespace donner::gpu::browser
