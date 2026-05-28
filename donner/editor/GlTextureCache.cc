#include "donner/editor/GlTextureCache.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <utility>

#include "donner/editor/TracyWrapper.h"
#ifdef DONNER_EDITOR_WGPU
#include "backends/imgui_impl_wgpu.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#endif

namespace donner::editor {

namespace {

#ifdef DONNER_EDITOR_WGPU
constexpr std::size_t kRetiredSnapshotFrameLimit = 3;
constexpr uint32_t kWgpuBytesPerRowAlignment = 256u;

uint32_t AlignWgpuBytesPerRow(uint32_t value) {
  return (value + kWgpuBytesPerRowAlignment - 1u) & ~(kWgpuBytesPerRowAlignment - 1u);
}

ImTextureID TextureViewToImTextureId(const wgpu::TextureView& textureView) {
  const WGPUTextureView rawTextureView = textureView;
  return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(rawTextureView));
}
#endif

Vector2i PayloadDimensionsForTile(const RenderResult::CompositedTile& tile) {
  if (!tile.bitmap.empty()) {
    return tile.bitmap.dimensions;
  }
  if (tile.textureSnapshot != nullptr) {
    return tile.textureSnapshot->dimensions();
  }
  return tile.bitmapDimsPx;
}

bool TileHasPayload(const RenderResult::CompositedTile& tile) {
  return !tile.bitmap.empty() || tile.textureSnapshot != nullptr;
}

std::uint64_t PixelArea(const Vector2i& dimensions) {
  if (dimensions.x <= 0 || dimensions.y <= 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(dimensions.x) * static_cast<std::uint64_t>(dimensions.y);
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace

#ifdef DONNER_EDITOR_WGPU
struct GlTextureCache::WgpuUploadedTexture {
  donner::geode::ScopedWgpuHandle<wgpu::Texture> texture;
  donner::geode::ScopedWgpuHandle<wgpu::TextureView> view;
  Vector2i allocationDimensions = Vector2i::Zero();
};
#endif

GlTextureCache::GlTextureCache(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice)
#ifdef DONNER_EDITOR_WGPU
    : geodeDevice_(std::move(geodeDevice))
#endif
{
#ifndef DONNER_EDITOR_WGPU
  (void)geodeDevice;
#endif
}

CompositedTileTextureIdentity TextureIdentityForCompositedTile(
    const RenderResult::CompositedTile& tile) {
  return CompositedTileTextureIdentity{
      .kind = tile.kind,
      .generation = tile.generation,
      .textureDimsPx = PayloadDimensionsForTile(tile),
      .rasterCanvasSize = tile.rasterCanvasSize,
  };
}

bool TextureIdentityMatchesCompositedTile(const CompositedTileTextureIdentity& cachedIdentity,
                                          const RenderResult::CompositedTile& tile) {
  return cachedIdentity == TextureIdentityForCompositedTile(tile);
}

Vector2i PowerOfTwoTextureDimensionsForPayload(const Vector2i& payloadDimensions) {
  if (payloadDimensions.x <= 0 || payloadDimensions.y <= 0) {
    return Vector2i::Zero();
  }

  const auto nextPowerOfTwo = [](int value) {
    constexpr int kMaxSafePowerOfTwo = 1 << 30;
    if (value > kMaxSafePowerOfTwo) {
      return value;
    }

    int result = 1;
    while (result < value) {
      result <<= 1;
    }
    return result;
  };

  return Vector2i(nextPowerOfTwo(payloadDimensions.x), nextPowerOfTwo(payloadDimensions.y));
}

Vector2d TextureUvBottomRightForPayload(const Vector2i& payloadDimensions,
                                        const Vector2i& allocationDimensions) {
  if (payloadDimensions.x <= 0 || payloadDimensions.y <= 0 || allocationDimensions.x <= 0 ||
      allocationDimensions.y <= 0) {
    return Vector2d(1.0, 1.0);
  }

  return Vector2d(
      static_cast<double>(payloadDimensions.x) / static_cast<double>(allocationDimensions.x),
      static_cast<double>(payloadDimensions.y) / static_cast<double>(allocationDimensions.y));
}

std::uint64_t BitmapPayloadBytes(const svg::RendererBitmap& bitmap) {
  if (bitmap.empty() || bitmap.rowBytes == 0u || bitmap.dimensions.y <= 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(bitmap.rowBytes) *
         static_cast<std::uint64_t>(bitmap.dimensions.y);
}

std::uint64_t TexturePayloadBytes(const Vector2i& dimensions) {
  return PixelArea(dimensions) * 4u;
}

FrameCostBreakdown::CompositedUpload CostForCompositedPreviewUpload(
    const RenderResult::CompositedPreview& preview) {
  FrameCostBreakdown::CompositedUpload cost;
  cost.tileCount = static_cast<int>(preview.tiles.size());
  for (const RenderResult::CompositedTile& tile : preview.tiles) {
    if (tile.kind == RenderResult::CompositedTile::Kind::Immediate) {
      ++cost.immediateTileCount;
    }
    const Vector2i payloadDimensions = PayloadDimensionsForTile(tile);
    cost.tilePixelArea += PixelArea(payloadDimensions);

    if (!tile.bitmap.empty()) {
      ++cost.payloadTileCount;
      ++cost.bitmapPayloadTileCount;
      cost.payloadBytes += BitmapPayloadBytes(tile.bitmap);
      cost.payloadPixelArea += PixelArea(tile.bitmap.dimensions);
    } else if (tile.textureSnapshot != nullptr) {
      ++cost.payloadTileCount;
      ++cost.texturePayloadTileCount;
      const Vector2i textureDimensions = tile.textureSnapshot->dimensions();
      cost.payloadBytes += TexturePayloadBytes(textureDimensions);
      cost.payloadPixelArea += PixelArea(textureDimensions);
    } else {
      ++cost.metadataOnlyTileCount;
    }
  }
  return cost;
}

GlTextureCache::~GlTextureCache() {
#ifdef DONNER_EDITOR_WGPU
  releaseImGuiTexture(overlayTexture_);
  for (const auto& [_, entry] : tileTextures_) {
    releaseImGuiTexture(entry.texture);
  }
  for (const auto& [_, entry] : overviewTileTextures_) {
    releaseImGuiTexture(entry.texture);
  }
  for (const RetiredSnapshot& retired : pendingRetiredSnapshots_) {
    releaseImGuiTexture(retired.texture);
  }
  for (const RetiredSnapshotBatch& batch : retiredSnapshotFrames_) {
    for (const RetiredSnapshot& retired : batch) {
      releaseImGuiTexture(retired.texture);
    }
  }
#else
  if (overlayTexture_ != 0) {
    glDeleteTextures(1, &overlayTexture_);
  }
  for (auto& [_, entry] : tileTextures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
  for (auto& [_, entry] : overviewTileTextures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
#endif
}

void GlTextureCache::initialize() {
#ifndef DONNER_EDITOR_WGPU
  if (overlayTexture_ == 0) {
    glGenTextures(1, &overlayTexture_);
    InitializeTexture(overlayTexture_);
  }
#endif
}

void GlTextureCache::uploadOverlay(const svg::RendererBitmap& bitmap) {
  ZoneScopedN("GlTextureCache::uploadOverlay");
  const auto uploadStart = std::chrono::steady_clock::now();
  overlayScreenRect_.reset();
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
  const std::shared_ptr<WgpuUploadedTexture> reusableTexture =
      overlayTextureSnapshot_ == nullptr ? overlayUploadedTexture_ : nullptr;
  std::shared_ptr<WgpuUploadedTexture> uploadedTexture =
      uploadBitmapToWgpu(bitmap, reusableTexture);
  const bool reusedTexture = uploadedTexture != nullptr &&
                             uploadedTexture == overlayUploadedTexture_ &&
                             overlayTextureSnapshot_ == nullptr;
  if (overlayTexture_ != 0 && !reusedTexture) {
    retiredSnapshots.push_back(RetiredSnapshot{
        .texture = overlayTexture_,
        .snapshot = std::move(overlayTextureSnapshot_),
        .uploadedTexture = std::move(overlayUploadedTexture_),
    });
  }
  overlayTextureSnapshot_.reset();
  overlayUploadedTexture_ = std::move(uploadedTexture);
  overlayTexture_ = overlayUploadedTexture_ != nullptr
                        ? TextureViewToImTextureId(overlayUploadedTexture_->view.get())
                        : 0;
  if (overlayTexture_ != 0) {
    overlayWidth_ = bitmap.dimensions.x;
    overlayHeight_ = bitmap.dimensions.y;
    overlayAllocatedWidth_ = overlayUploadedTexture_->allocationDimensions.x;
    overlayAllocatedHeight_ = overlayUploadedTexture_->allocationDimensions.y;
    overlayUvBottomRight_ = TextureUvBottomRightForPayload(
        bitmap.dimensions, overlayUploadedTexture_->allocationDimensions);
  } else {
    overlayWidth_ = 0;
    overlayHeight_ = 0;
    overlayAllocatedWidth_ = 0;
    overlayAllocatedHeight_ = 0;
    overlayUvBottomRight_ = Vector2d(1.0, 1.0);
  }
  retireSnapshots(std::move(retiredSnapshots));
#else
  UploadBitmap(overlayTexture_, bitmap, &overlayWidth_, &overlayHeight_, &overlayAllocatedWidth_,
               &overlayAllocatedHeight_, &overlayUvBottomRight_);
#endif
  lastOverlayUploadCost_ = FrameCostBreakdown::Overlay{
      .uploadMs = MillisecondsSince(uploadStart),
      .payloadBytes = BitmapPayloadBytes(bitmap),
      .canvasSize = Vector2i(overlayWidth_, overlayHeight_),
  };
}

void GlTextureCache::uploadOverlayTexture(
    std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot) {
  ZoneScopedN("GlTextureCache::uploadOverlayTexture");
#ifdef DONNER_EDITOR_WGPU
  const Vector2i inputDimensions =
      textureSnapshot != nullptr ? textureSnapshot->dimensions() : Vector2i::Zero();
#else
  const Vector2i inputDimensions = Vector2i::Zero();
#endif
  const auto uploadStart = std::chrono::steady_clock::now();
  overlayScreenRect_.reset();
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
  const bool acquiredSnapshot =
      textureSnapshot != nullptr && overlayTextureSnapshot_ != textureSnapshot;
  if (overlayTexture_ != 0 &&
      (overlayTextureSnapshot_ != textureSnapshot || overlayUploadedTexture_ != nullptr)) {
    retiredSnapshots.push_back(RetiredSnapshot{
        .texture = overlayTexture_,
        .snapshot = std::move(overlayTextureSnapshot_),
        .uploadedTexture = std::move(overlayUploadedTexture_),
    });
  }
  overlayUploadedTexture_.reset();
  overlayTextureSnapshot_ = std::move(textureSnapshot);
  overlayTexture_ = ToImTextureId(overlayTextureSnapshot_.get());
  if (overlayTextureSnapshot_ != nullptr && overlayTexture_ != 0) {
    if (acquiredSnapshot) {
      ImGui_ImplWGPU_AddTexturePremultipliedAlphaRef(overlayTexture_);
    }
    const Vector2i dims = overlayTextureSnapshot_->dimensions();
    overlayWidth_ = dims.x;
    overlayHeight_ = dims.y;
    overlayAllocatedWidth_ = dims.x;
    overlayAllocatedHeight_ = dims.y;
    overlayUvBottomRight_ = TextureUvBottomRightForPayload(dims, dims);
  } else {
    overlayTextureSnapshot_.reset();
    overlayTexture_ = 0;
    overlayWidth_ = 0;
    overlayHeight_ = 0;
    overlayAllocatedWidth_ = 0;
    overlayAllocatedHeight_ = 0;
    overlayUvBottomRight_ = Vector2d(1.0, 1.0);
  }
  retireSnapshots(std::move(retiredSnapshots));
#else
  (void)textureSnapshot;
  clearOverlay();
#endif
  lastOverlayUploadCost_ = FrameCostBreakdown::Overlay{
      .uploadMs = MillisecondsSince(uploadStart),
      .payloadBytes = TexturePayloadBytes(inputDimensions),
      .canvasSize = Vector2i(overlayWidth_, overlayHeight_),
  };
}

void GlTextureCache::uploadComposited(const RenderResult::CompositedPreview& preview,
                                      std::optional<EditorRasterViewport> rasterViewport) {
  ZoneScopedN("GlTextureCache::uploadComposited");
  const auto uploadStart = std::chrono::steady_clock::now();
  lastCompositedUploadCost_ = CostForCompositedPreviewUpload(preview);
  activeTilesViewportBounded_ = rasterViewport.has_value() && rasterViewport->viewportBounded;
  const bool retainAsOverview = rasterViewport.has_value() && !rasterViewport->viewportBounded;

  // Track which ids appear in the new snapshot so we can evict
  // textures whose tile has disappeared (drag-target switch demoted a
  // layer, segment cache shrank, etc.).
  std::unordered_set<std::string> liveIds;
  liveIds.reserve(preview.tiles.size());

  tiles_.clear();
  tiles_.reserve(preview.tiles.size());
  metadataOnlyMissCount_ = 0;
  duplicateLiveTextureCount_ = 0;

#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
#endif

  const auto makeTileView = [](const RenderResult::CompositedTile& tile,
                               const CachedTextureEntry& entry, ImTextureID texture,
                               bool metadataOnly) {
    TileView view;
    view.texture = texture;
    view.id = tile.id;
    view.kind = tile.kind;
    view.layerEntity = tile.layerEntity;
    view.generation = tile.generation;
    view.bitmapDimsPx =
        !tile.bitmap.empty() ? tile.bitmap.dimensions : Vector2i(entry.width, entry.height);
    view.rasterCanvasSize = tile.rasterCanvasSize;
    view.canvasOffsetDoc = tile.canvasOffsetDoc;
    view.bitmapDimsDoc = tile.bitmapDimsDoc;
    view.dragTranslationDoc = tile.dragTranslationDoc;
    view.uvBottomRight = entry.uvBottomRight;
    view.documentFromCachedDocument = tile.documentFromCachedDocument;
    view.metadataOnly = metadataOnly;
    view.isDragTarget = tile.isDragTarget;
    return view;
  };

  const auto uploadTilePayloadToEntry = [&](const RenderResult::CompositedTile& tile,
                                            CachedTextureEntry* entry) {
    if (entry == nullptr) {
      return false;
    }

    const CompositedTileTextureIdentity tileIdentity = TextureIdentityForCompositedTile(tile);
#ifdef DONNER_EDITOR_WGPU
    if (entry->texture != 0 && entry->identity == tileIdentity) {
      return true;
    }

    NativeTextureHandle textureId = 0;
    Vector2i textureDims = Vector2i::Zero();
    Vector2i allocationDims = Vector2i::Zero();
    Vector2d uvBottomRight(1.0, 1.0);
    std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot;
    std::shared_ptr<WgpuUploadedTexture> uploadedTexture;
    bool reusedTexture = false;

    if (tile.textureSnapshot != nullptr) {
      textureId = ToImTextureId(tile.textureSnapshot.get());
      textureDims = tile.textureSnapshot->dimensions();
      allocationDims = textureDims;
      uvBottomRight = TextureUvBottomRightForPayload(textureDims, allocationDims);
      textureSnapshot = tile.textureSnapshot;
    } else if (!tile.bitmap.empty()) {
      const std::shared_ptr<WgpuUploadedTexture> reusableTexture =
          entry->textureSnapshot == nullptr ? entry->uploadedTexture : nullptr;
      uploadedTexture = uploadBitmapToWgpu(tile.bitmap, reusableTexture);
      if (uploadedTexture != nullptr) {
        textureId = TextureViewToImTextureId(uploadedTexture->view.get());
        textureDims = tile.bitmap.dimensions;
        allocationDims = uploadedTexture->allocationDimensions;
        uvBottomRight = TextureUvBottomRightForPayload(textureDims, allocationDims);
        reusedTexture = textureId == entry->texture && uploadedTexture == entry->uploadedTexture &&
                        entry->textureSnapshot == nullptr;
      }
    }

    if (textureId == 0) {
      return false;
    }

    if (entry->texture != 0 && !reusedTexture) {
      retiredSnapshots.push_back(RetiredSnapshot{
          .texture = entry->texture,
          .snapshot = std::move(entry->textureSnapshot),
          .uploadedTexture = std::move(entry->uploadedTexture),
      });
    }
    if (textureSnapshot != nullptr) {
      ImGui_ImplWGPU_AddTexturePremultipliedAlphaRef(textureId);
    }
    entry->texture = textureId;
    entry->textureSnapshot = std::move(textureSnapshot);
    entry->uploadedTexture = std::move(uploadedTexture);
    entry->identity = tileIdentity;
    entry->uploadedGeneration = tile.generation;
    entry->width = textureDims.x;
    entry->height = textureDims.y;
    entry->allocatedWidth = allocationDims.x;
    entry->allocatedHeight = allocationDims.y;
    entry->uvBottomRight = uvBottomRight;
    return true;
#else
    if (tile.bitmap.empty()) {
      return false;
    }
    if (entry->texture == 0) {
      glGenTextures(1, &entry->texture);
      InitializeTexture(entry->texture);
    }

    const bool needsUpload = entry->identity != tileIdentity;
    if (needsUpload) {
      UploadBitmap(entry->texture, tile.bitmap, &entry->width, &entry->height,
                   &entry->allocatedWidth, &entry->allocatedHeight, &entry->uvBottomRight);
      entry->identity = tileIdentity;
      entry->uploadedGeneration = tile.generation;
    }
    return entry->texture != 0;
#endif
  };

  for (const auto& tile : preview.tiles) {
    // Partial tile snapshots can carry metadata without a pixel payload.
    // Preserve the already-uploaded texture for that tile id and update only
    // its presentation geometry.
    if (!TileHasPayload(tile)) {
      auto it = tileTextures_.find(tile.id);
      if (it == tileTextures_.end() ||
          !TextureIdentityMatchesCompositedTile(it->second.identity, tile)) {
        ++metadataOnlyMissCount_;
        continue;
      }
      liveIds.insert(tile.id);
      tiles_.push_back(makeTileView(tile, it->second, ToImTextureId(it->second.texture),
                                    /*metadataOnly=*/true));
      continue;
    }

    auto& entry = tileTextures_[tile.id];
    if (!uploadTilePayloadToEntry(tile, &entry)) {
      continue;
    }
    liveIds.insert(tile.id);
    tiles_.push_back(makeTileView(tile, entry, ToImTextureId(entry.texture),
                                  /*metadataOnly=*/false));
  }

  // Evict textures whose tile id no longer appears.
  for (auto it = tileTextures_.begin(); it != tileTextures_.end();) {
    if (liveIds.find(it->first) == liveIds.end()) {
#ifndef DONNER_EDITOR_WGPU
      if (it->second.texture != 0) {
        glDeleteTextures(1, &it->second.texture);
      }
#else
      if (it->second.texture != 0) {
        retiredSnapshots.push_back(RetiredSnapshot{
            .texture = it->second.texture,
            .snapshot = std::move(it->second.textureSnapshot),
            .uploadedTexture = std::move(it->second.uploadedTexture),
        });
      }
#endif
      it = tileTextures_.erase(it);
    } else {
      ++it;
    }
  }

  if (retainAsOverview) {
    std::unordered_set<std::string> liveOverviewIds;
    liveOverviewIds.reserve(preview.tiles.size());
    overviewTiles_.clear();
    overviewTiles_.reserve(preview.tiles.size());

    for (const RenderResult::CompositedTile& tile : preview.tiles) {
      if (!TileHasPayload(tile)) {
        continue;
      }

      auto& entry = overviewTileTextures_[tile.id];
      if (!uploadTilePayloadToEntry(tile, &entry)) {
        continue;
      }
      liveOverviewIds.insert(tile.id);
      overviewTiles_.push_back(makeTileView(tile, entry, ToImTextureId(entry.texture),
                                            /*metadataOnly=*/false));
    }

    for (auto it = overviewTileTextures_.begin(); it != overviewTileTextures_.end();) {
      if (liveOverviewIds.find(it->first) == liveOverviewIds.end()) {
#ifndef DONNER_EDITOR_WGPU
        if (it->second.texture != 0) {
          glDeleteTextures(1, &it->second.texture);
        }
#else
        if (it->second.texture != 0) {
          retiredSnapshots.push_back(RetiredSnapshot{
              .texture = it->second.texture,
              .snapshot = std::move(it->second.textureSnapshot),
              .uploadedTexture = std::move(it->second.uploadedTexture),
          });
        }
#endif
        it = overviewTileTextures_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::unordered_map<ImTextureID, std::string> liveTextureOwners;
  liveTextureOwners.reserve(tiles_.size());
  for (const TileView& tile : tiles_) {
    if (tile.texture == 0) {
      continue;
    }
    auto [ownerIt, inserted] = liveTextureOwners.emplace(tile.texture, tile.id);
    if (!inserted && ownerIt->second != tile.id) {
      ++duplicateLiveTextureCount_;
    }
  }

#ifdef DONNER_EDITOR_WGPU
  retireSnapshots(std::move(retiredSnapshots));
#endif
  lastCompositedUploadCost_.uploadMs = MillisecondsSince(uploadStart);
}

void GlTextureCache::advancePresentationFrame() {
#ifdef DONNER_EDITOR_WGPU
  if (!pendingRetiredSnapshots_.empty()) {
    retiredSnapshotFrames_.push_back(std::move(pendingRetiredSnapshots_));
    pendingRetiredSnapshots_.clear();
  } else if (!retiredSnapshotFrames_.empty()) {
    retiredSnapshotFrames_.push_back(RetiredSnapshotBatch{});
  }

  while (retiredSnapshotFrames_.size() > kRetiredSnapshotFrameLimit) {
    for (const RetiredSnapshot& retired : retiredSnapshotFrames_.front()) {
      releaseImGuiTexture(retired.texture);
    }
    retiredSnapshotFrames_.pop_front();
  }
#endif
}

void GlTextureCache::clearOverlay() {
  const auto uploadStart = std::chrono::steady_clock::now();
  overlayScreenRect_.reset();
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
  if (overlayTexture_ != 0) {
    retiredSnapshots.push_back(RetiredSnapshot{
        .texture = overlayTexture_,
        .snapshot = std::move(overlayTextureSnapshot_),
        .uploadedTexture = std::move(overlayUploadedTexture_),
    });
  }
  overlayTextureSnapshot_.reset();
  overlayUploadedTexture_.reset();
  retireSnapshots(std::move(retiredSnapshots));
#else
  overlayTextureSnapshot_.reset();
#endif
  overlayTexture_ = 0;
  overlayWidth_ = 0;
  overlayHeight_ = 0;
  overlayAllocatedWidth_ = 0;
  overlayAllocatedHeight_ = 0;
  overlayUvBottomRight_ = Vector2d(1.0, 1.0);
  lastOverlayUploadCost_ = FrameCostBreakdown::Overlay{
      .uploadMs = MillisecondsSince(uploadStart),
  };
}

void GlTextureCache::resetComposited() {
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
  retiredSnapshots.reserve(tileTextures_.size() + overviewTileTextures_.size());
  const auto retireEntry = [&](CachedTextureEntry& entry) {
    if (entry.texture != 0) {
      retiredSnapshots.push_back(RetiredSnapshot{
          .texture = entry.texture,
          .snapshot = std::move(entry.textureSnapshot),
          .uploadedTexture = std::move(entry.uploadedTexture),
      });
    }
  };
  for (auto& [_, entry] : tileTextures_) {
    retireEntry(entry);
  }
  for (auto& [_, entry] : overviewTileTextures_) {
    retireEntry(entry);
  }
  retireSnapshots(std::move(retiredSnapshots));
#else
  for (auto& [_, entry] : tileTextures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
  for (auto& [_, entry] : overviewTileTextures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
#endif
  tileTextures_.clear();
  overviewTileTextures_.clear();
  tiles_.clear();
  overviewTiles_.clear();
  activeTilesViewportBounded_ = false;
  metadataOnlyMissCount_ = 0;
  duplicateLiveTextureCount_ = 0;
  lastCompositedUploadCost_ = FrameCostBreakdown::CompositedUpload{};
}

PresentationResourceStats GlTextureCache::presentationResourceStats() const {
  PresentationResourceStats stats;

  const auto updateLargest = [&](const Vector2i& dimensions) {
    if (PixelArea(dimensions) > PixelArea(stats.largestAllocationPx)) {
      stats.largestAllocationPx = dimensions;
    }
  };

  const auto allocationBytes = [&](const Vector2i& dimensions) {
    updateLargest(dimensions);
    return TexturePayloadBytes(dimensions);
  };

  const auto cachedEntryBytes = [&](const CachedTextureEntry& entry) {
#ifdef DONNER_EDITOR_WGPU
    if (entry.uploadedTexture != nullptr) {
      return allocationBytes(entry.uploadedTexture->allocationDimensions);
    }
#endif
    if (entry.allocatedWidth > 0 && entry.allocatedHeight > 0) {
      return allocationBytes(Vector2i(entry.allocatedWidth, entry.allocatedHeight));
    }
    if (entry.textureSnapshot != nullptr) {
      return allocationBytes(entry.textureSnapshot->dimensions());
    }
    return allocationBytes(Vector2i(entry.width, entry.height));
  };

  if (overlayTexture_ != 0) {
#ifdef DONNER_EDITOR_WGPU
    if (overlayUploadedTexture_ != nullptr) {
      stats.overlayBytes = allocationBytes(overlayUploadedTexture_->allocationDimensions);
    } else
#endif
        if (overlayTextureSnapshot_ != nullptr) {
      stats.overlayBytes = allocationBytes(overlayTextureSnapshot_->dimensions());
    } else if (overlayAllocatedWidth_ > 0 && overlayAllocatedHeight_ > 0) {
      stats.overlayBytes =
          allocationBytes(Vector2i(overlayAllocatedWidth_, overlayAllocatedHeight_));
    } else {
      stats.overlayBytes = allocationBytes(Vector2i(overlayWidth_, overlayHeight_));
    }
  }

  stats.activeTileTextures = static_cast<int>(tileTextures_.size());
  for (const auto& [_, entry] : tileTextures_) {
    if (entry.texture != 0) {
      stats.activeTileBytes += cachedEntryBytes(entry);
    }
  }

  stats.overviewTileTextures = static_cast<int>(overviewTileTextures_.size());
  for (const auto& [_, entry] : overviewTileTextures_) {
    if (entry.texture != 0) {
      stats.overviewTileBytes += cachedEntryBytes(entry);
    }
  }

#ifdef DONNER_EDITOR_WGPU
  const auto retiredBytes = [&](const RetiredSnapshot& retired) {
    if (retired.uploadedTexture != nullptr) {
      return allocationBytes(retired.uploadedTexture->allocationDimensions);
    }
    if (retired.snapshot != nullptr) {
      return allocationBytes(retired.snapshot->dimensions());
    }
    return std::uint64_t{0};
  };

  stats.pendingRetiredTextures = static_cast<int>(pendingRetiredSnapshots_.size());
  for (const RetiredSnapshot& retired : pendingRetiredSnapshots_) {
    if (retired.texture != 0) {
      stats.pendingRetiredBytes += retiredBytes(retired);
    }
  }

  stats.retiredFrameCount = static_cast<int>(retiredSnapshotFrames_.size());
  for (const RetiredSnapshotBatch& batch : retiredSnapshotFrames_) {
    for (const RetiredSnapshot& retired : batch) {
      if (retired.texture != 0) {
        ++stats.agedRetiredTextures;
        stats.agedRetiredBytes += retiredBytes(retired);
      }
    }
  }

  if (geodeDevice_ != nullptr) {
    stats.wgpuLifetimeTextureCreates = geodeDevice_->lifetimeTextureCreates();
    stats.wgpuLifetimeBufferCreates = geodeDevice_->lifetimeBufferCreates();
  }
#endif

  stats.totalTrackedBytes = stats.overlayBytes + stats.activeTileBytes + stats.overviewTileBytes +
                            stats.pendingRetiredBytes + stats.agedRetiredBytes;
  peakTrackedResourceBytes_ = std::max(peakTrackedResourceBytes_, stats.totalTrackedBytes);
  stats.peakTrackedBytes = peakTrackedResourceBytes_;
  return stats;
}

ImTextureID GlTextureCache::overlayTexture() const {
  return ToImTextureId(overlayTexture_);
}

void GlTextureCache::setOverlayScreenRect(std::optional<Box2d> screenRect) {
  if (overlayWidth_ <= 0 || overlayHeight_ <= 0) {
    overlayScreenRect_.reset();
    return;
  }

  overlayScreenRect_ = std::move(screenRect);
}

ImTextureID GlTextureCache::ToImTextureId(NativeTextureHandle texture) {
#ifdef DONNER_EDITOR_WGPU
  return texture;
#else
  return static_cast<ImTextureID>(texture);
#endif
}

#ifdef DONNER_EDITOR_WGPU
GlTextureCache::RetiredSnapshot GlTextureCache::RetireSnapshot(
    NativeTextureHandle texture, std::shared_ptr<const svg::RendererTextureSnapshot> snapshot) {
  return RetiredSnapshot{
      .texture = texture,
      .snapshot = std::move(snapshot),
  };
}

void GlTextureCache::releaseImGuiTexture(NativeTextureHandle texture) {
  if (texture == 0) {
    return;
  }

  ImGui_ImplWGPU_RemoveTexturePremultipliedAlphaRef(texture);
  ImGui_ImplWGPU_RemoveTexture(texture);
}

ImTextureID GlTextureCache::ToImTextureId(const svg::RendererTextureSnapshot* textureSnapshot) {
  if (textureSnapshot == nullptr ||
      textureSnapshot->backend() != svg::RendererTextureSnapshotBackend::Geode) {
    return 0;
  }
  const auto* geodeTexture = static_cast<const svg::RendererGeodeTextureSnapshot*>(textureSnapshot);
  return TextureViewToImTextureId(geodeTexture->textureView());
}

std::shared_ptr<GlTextureCache::WgpuUploadedTexture> GlTextureCache::uploadBitmapToWgpu(
    const svg::RendererBitmap& bitmap,
    const std::shared_ptr<WgpuUploadedTexture>& reusableTexture) {
  if (geodeDevice_ == nullptr || bitmap.empty() || bitmap.rowBytes == 0u) {
    return nullptr;
  }

  const uint32_t width = static_cast<uint32_t>(bitmap.dimensions.x);
  const uint32_t height = static_cast<uint32_t>(bitmap.dimensions.y);
  const Vector2i allocationDimensions = PowerOfTwoTextureDimensionsForPayload(bitmap.dimensions);
  if (allocationDimensions.x <= 0 || allocationDimensions.y <= 0) {
    return nullptr;
  }
  const uint32_t allocationWidth = static_cast<uint32_t>(allocationDimensions.x);
  const uint32_t allocationHeight = static_cast<uint32_t>(allocationDimensions.y);
  const uint32_t tightBytesPerRow = width * 4u;
  const uint32_t paddedBytesPerRow = AlignWgpuBytesPerRow(tightBytesPerRow);

  if (bitmap.rowBytes < tightBytesPerRow ||
      bitmap.pixels.size() < bitmap.rowBytes * static_cast<std::size_t>(height)) {
    return nullptr;
  }

  std::shared_ptr<WgpuUploadedTexture> uploaded = reusableTexture;
  if (uploaded == nullptr || uploaded->allocationDimensions != allocationDimensions ||
      !uploaded->texture || !uploaded->view) {
    wgpu::TextureDescriptor textureDesc = {};
    textureDesc.label = donner::geode::wgpuLabel("EditorUploadedBitmap");
    textureDesc.size = {allocationWidth, allocationHeight, 1};
    textureDesc.mipLevelCount = 1;
    textureDesc.sampleCount = 1;
    textureDesc.dimension = wgpu::TextureDimension::_2D;
    textureDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    textureDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;

    uploaded = std::make_shared<WgpuUploadedTexture>();
    uploaded->texture.reset(geodeDevice_->device().createTexture(textureDesc));
    if (!uploaded->texture) {
      return nullptr;
    }
    uploaded->allocationDimensions = allocationDimensions;
    geodeDevice_->countTexture();
  }

  std::vector<uint8_t> uploadPixels;
  const uint8_t* uploadData = bitmap.pixels.data();
  std::size_t uploadSize = bitmap.rowBytes * static_cast<std::size_t>(height);
  if (bitmap.rowBytes != tightBytesPerRow || tightBytesPerRow != paddedBytesPerRow) {
    uploadPixels.assign(static_cast<std::size_t>(paddedBytesPerRow) * height, 0u);
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* src = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
      uint8_t* dst = uploadPixels.data() + static_cast<std::size_t>(y) * paddedBytesPerRow;
      std::memcpy(dst, src, tightBytesPerRow);
    }
    uploadData = uploadPixels.data();
    uploadSize = uploadPixels.size();
  }

  wgpu::TexelCopyTextureInfo dst = {};
  dst.texture = uploaded->texture.get();
  dst.mipLevel = 0;
  dst.origin = {0, 0, 0};
  dst.aspect = wgpu::TextureAspect::All;

  wgpu::TexelCopyBufferLayout layout = {};
  layout.offset = 0;
  layout.bytesPerRow = paddedBytesPerRow;
  layout.rowsPerImage = height;

  wgpu::Extent3D writeSize = {width, height, 1};
  geodeDevice_->queue().writeTexture(dst, uploadData, uploadSize, layout, writeSize);

  const auto pixelAt = [&](uint32_t x, uint32_t y) {
    return bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes +
           static_cast<std::size_t>(x) * 4u;
  };

  if (allocationWidth > width) {
    constexpr uint32_t kColumnBytesPerRow = kWgpuBytesPerRowAlignment;
    std::vector<uint8_t> edgeColumn(static_cast<std::size_t>(kColumnBytesPerRow) * height, 0u);
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(edgeColumn.data() + static_cast<std::size_t>(y) * kColumnBytesPerRow,
                  pixelAt(width - 1u, y), 4u);
    }

    wgpu::TexelCopyTextureInfo edgeDst = dst;
    edgeDst.origin = {width, 0, 0};
    wgpu::TexelCopyBufferLayout edgeLayout = {};
    edgeLayout.offset = 0;
    edgeLayout.bytesPerRow = kColumnBytesPerRow;
    edgeLayout.rowsPerImage = height;
    geodeDevice_->queue().writeTexture(edgeDst, edgeColumn.data(), edgeColumn.size(), edgeLayout,
                                       wgpu::Extent3D{1, height, 1});
  }

  if (allocationHeight > height) {
    std::vector<uint8_t> edgeRow(static_cast<std::size_t>(paddedBytesPerRow), 0u);
    std::memcpy(edgeRow.data(), pixelAt(0, height - 1u), tightBytesPerRow);

    wgpu::TexelCopyTextureInfo edgeDst = dst;
    edgeDst.origin = {0, height, 0};
    wgpu::TexelCopyBufferLayout edgeLayout = {};
    edgeLayout.offset = 0;
    edgeLayout.bytesPerRow = paddedBytesPerRow;
    edgeLayout.rowsPerImage = 1;
    geodeDevice_->queue().writeTexture(edgeDst, edgeRow.data(), edgeRow.size(), edgeLayout,
                                       wgpu::Extent3D{width, 1, 1});
  }

  if (allocationWidth > width && allocationHeight > height) {
    constexpr uint32_t kPixelBytesPerRow = kWgpuBytesPerRowAlignment;
    std::vector<uint8_t> edgePixel(kPixelBytesPerRow, 0u);
    std::memcpy(edgePixel.data(), pixelAt(width - 1u, height - 1u), 4u);

    wgpu::TexelCopyTextureInfo edgeDst = dst;
    edgeDst.origin = {width, height, 0};
    wgpu::TexelCopyBufferLayout edgeLayout = {};
    edgeLayout.offset = 0;
    edgeLayout.bytesPerRow = kPixelBytesPerRow;
    edgeLayout.rowsPerImage = 1;
    geodeDevice_->queue().writeTexture(edgeDst, edgePixel.data(), edgePixel.size(), edgeLayout,
                                       wgpu::Extent3D{1, 1, 1});
  }

  if (!uploaded->view) {
    uploaded->view.reset(uploaded->texture.get().createView());
  }
  if (!uploaded->view) {
    return nullptr;
  }
  return uploaded;
}

void GlTextureCache::retireSnapshots(RetiredSnapshotBatch snapshots) {
  if (snapshots.empty()) {
    return;
  }

  pendingRetiredSnapshots_.insert(pendingRetiredSnapshots_.end(),
                                  std::make_move_iterator(snapshots.begin()),
                                  std::make_move_iterator(snapshots.end()));
}
#endif

#ifndef DONNER_EDITOR_WGPU
void GlTextureCache::UploadBitmap(GLuint texture, const svg::RendererBitmap& bitmap, int* outWidth,
                                  int* outHeight, int* outAllocatedWidth, int* outAllocatedHeight,
                                  Vector2d* outUvBottomRight) {
  if (bitmap.empty()) {
    *outWidth = 0;
    *outHeight = 0;
    *outAllocatedWidth = 0;
    *outAllocatedHeight = 0;
    *outUvBottomRight = Vector2d(1.0, 1.0);
    return;
  }

  const Vector2i allocationDimensions = PowerOfTwoTextureDimensionsForPayload(bitmap.dimensions);
  glBindTexture(GL_TEXTURE_2D, texture);
  if (*outAllocatedWidth != allocationDimensions.x ||
      *outAllocatedHeight != allocationDimensions.y) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, allocationDimensions.x, allocationDimensions.y, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    *outAllocatedWidth = allocationDimensions.x;
    *outAllocatedHeight = allocationDimensions.y;
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bitmap.dimensions.x, bitmap.dimensions.y, GL_RGBA,
                  GL_UNSIGNED_BYTE, bitmap.pixels.data());
  if (allocationDimensions.y > bitmap.dimensions.y) {
    const uint8_t* bottomRow =
        bitmap.pixels.data() + static_cast<std::size_t>(bitmap.dimensions.y - 1) * bitmap.rowBytes;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, bitmap.dimensions.y, bitmap.dimensions.x, 1, GL_RGBA,
                    GL_UNSIGNED_BYTE, bottomRow);
  }
  if (allocationDimensions.x > bitmap.dimensions.x) {
    std::vector<uint8_t> edgeColumn(static_cast<std::size_t>(bitmap.dimensions.y) * 4u, 0u);
    for (int y = 0; y < bitmap.dimensions.y; ++y) {
      const uint8_t* src = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes +
                           static_cast<std::size_t>(bitmap.dimensions.x - 1) * 4u;
      std::memcpy(edgeColumn.data() + static_cast<std::size_t>(y) * 4u, src, 4u);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, bitmap.dimensions.x, 0, 1, bitmap.dimensions.y, GL_RGBA,
                    GL_UNSIGNED_BYTE, edgeColumn.data());

    if (allocationDimensions.y > bitmap.dimensions.y) {
      const uint8_t* bottomRight =
          bitmap.pixels.data() +
          static_cast<std::size_t>(bitmap.dimensions.y - 1) * bitmap.rowBytes +
          static_cast<std::size_t>(bitmap.dimensions.x - 1) * 4u;
      glTexSubImage2D(GL_TEXTURE_2D, 0, bitmap.dimensions.x, bitmap.dimensions.y, 1, 1, GL_RGBA,
                      GL_UNSIGNED_BYTE, bottomRight);
    }
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  *outWidth = bitmap.dimensions.x;
  *outHeight = bitmap.dimensions.y;
  *outUvBottomRight = TextureUvBottomRightForPayload(bitmap.dimensions, allocationDimensions);
}

void GlTextureCache::InitializeTexture(GLuint texture) {
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
#endif

}  // namespace donner::editor
