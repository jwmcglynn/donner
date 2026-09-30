#include "donner/editor/GlTextureCache.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <utility>

#include "donner/editor/CapturedPresentation.h"
#include "donner/editor/TracyWrapper.h"
#ifdef DONNER_EDITOR_WGPU
#include "donner/editor/RuntimeBitmapUpload.h"
#include "donner/editor/gui/ImGuiRuntimeRenderer.h"
#include "donner/editor/gui/UiTextureRegistration.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#endif

namespace donner::editor {

namespace {

#ifdef DONNER_EDITOR_WGPU
constexpr std::size_t kRetiredSnapshotFrameLimit = 3;
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

// Cheap content fingerprint for a thumbnail bitmap, used to decide whether a
// per-row thumbnail texture must be re-uploaded. FNV-1a over the valid pixel
// rows (honoring rowBytes) plus the dimensions. A collision only costs a missed
// re-upload of a same-size thumbnail, acceptable for a preview cell.
std::uint64_t ThumbnailBitmapFingerprint(const svg::RendererBitmap& bitmap) {
  std::uint64_t hash = 1469598103934665603ull;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  mix(static_cast<std::uint64_t>(bitmap.dimensions.x));
  mix(static_cast<std::uint64_t>(bitmap.dimensions.y));
  if (bitmap.empty() || bitmap.rowBytes == 0u || bitmap.dimensions.y <= 0) {
    return hash;
  }
  const std::size_t rowValidBytes =
      std::min<std::size_t>(bitmap.rowBytes, static_cast<std::size_t>(bitmap.dimensions.x) * 4u);
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const std::size_t rowStart = static_cast<std::size_t>(y) * bitmap.rowBytes;
    if (rowStart + rowValidBytes > bitmap.pixels.size()) {
      break;
    }
    const std::uint8_t* row = bitmap.pixels.data() + rowStart;
    for (std::size_t i = 0; i < rowValidBytes; ++i) {
      mix(row[i]);
    }
  }
  return hash;
}

}  // namespace

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
  for (const auto& [_, entry] : tileTextures_) {
    releaseCachedEntry(entry);
  }
  for (const auto& [_, entry] : overviewTileTextures_) {
    releaseCachedEntry(entry);
  }
  for (const auto& [_, entry] : thumbnailTextures_) {
    releaseCachedEntry(entry);
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
  for (auto& [_, entry] : tileTextures_) {
    releaseCachedEntry(entry);
  }
  for (auto& [_, entry] : overviewTileTextures_) {
    releaseCachedEntry(entry);
  }
  for (auto& [_, entry] : thumbnailTextures_) {
    releaseCachedEntry(entry);
  }
#endif
}

void GlTextureCache::releaseCachedEntry(const CachedTextureEntry& entry) {
#ifdef DONNER_EDITOR_WGPU
  releaseImGuiTexture(entry.texture);
#else
  if (entry.texture != 0 && entry.glTextureLifetime == nullptr) {
    glDeleteTextures(1, &entry.texture);
  }
#endif
}

void GlTextureCache::initialize() {}

GlTextureCache::TileView GlTextureCache::makeTileView(const RenderResult::CompositedTile& tile,
                                                      const CachedTextureEntry& entry) {
  return TileView{
      .uiTextureLifetime = entry.uiTextureLifetime,
      .texture = ToImTextureId(entry.texture),
      .id = tile.id,
      .kind = tile.kind,
      .layerEntity = tile.layerEntity,
      .generation = tile.generation,
      .bitmapDimsPx = Vector2i(entry.width, entry.height),
      .rasterCanvasSize = tile.rasterCanvasSize,
      .canvasOffsetDoc = tile.canvasOffsetDoc,
      .bitmapDimsDoc = tile.bitmapDimsDoc,
      .dragTranslationDoc = tile.dragTranslationDoc,
      .textureSnapshot = entry.textureSnapshot,
#ifndef DONNER_EDITOR_WGPU
      .glTextureLifetime = entry.glTextureLifetime,
#endif
      .uvBottomRight = entry.uvBottomRight,
      .documentFromCachedDocument = tile.documentFromCachedDocument,
      .metadataOnly = !TileHasPayload(tile),
      .isDragTarget = tile.isDragTarget,
  };
}

std::optional<GlTextureCache::CachedTextureEntry> GlTextureCache::uploadTilePayload(
    const RenderResult::CompositedTile& tile) {
  CachedTextureEntry entry;
  entry.identity = TextureIdentityForCompositedTile(tile);
  entry.uploadedGeneration = tile.generation;
#ifdef DONNER_EDITOR_WGPU
  Vector2i allocationDimensions;
  if (tile.textureSnapshot != nullptr) {
    entry.textureSnapshot = tile.textureSnapshot;
    allocationDimensions = tile.textureSnapshot->dimensions();
  } else if (!tile.bitmap.empty()) {
    const auto uploaded = uploadBitmapToWgpu(tile.bitmap);
    if (uploaded == nullptr) {
      return std::nullopt;
    }
    entry.textureSnapshot = uploaded;
    allocationDimensions = uploaded->allocationDimensions();
  }
  if (entry.textureSnapshot == nullptr) {
    return std::nullopt;
  }
  entry.texture = registerSnapshotTexture(*entry.textureSnapshot, &entry.uiTextureLifetime);
  if (entry.texture == 0 && HasUiTextureRegistry()) {
    return std::nullopt;
  }
  const Vector2i dimensions = entry.textureSnapshot->dimensions();
  entry.width = dimensions.x;
  entry.height = dimensions.y;
  entry.allocatedWidth = allocationDimensions.x;
  entry.allocatedHeight = allocationDimensions.y;
  entry.uvBottomRight = TextureUvBottomRightForPayload(dimensions, allocationDimensions);
#else
  if (tile.bitmap.empty()) {
    return std::nullopt;
  }
  glGenTextures(1, &entry.texture);
  if (entry.texture == 0) {
    return std::nullopt;
  }
  entry.glTextureLifetime =
      std::shared_ptr<const GLuint>(new GLuint(entry.texture), [](const GLuint* texture) {
        glDeleteTextures(1, texture);
        delete texture;
      });
  InitializeTexture(entry.texture);
  UploadBitmap(entry.texture, tile.bitmap, &entry.width, &entry.height, &entry.allocatedWidth,
               &entry.allocatedHeight, &entry.uvBottomRight);
#endif
  return entry;
}

void GlTextureCache::discardPreparedTiles(PreparedTileSet& prepared) {
  for (const auto& entry : prepared.newEntries) {
#ifdef DONNER_EDITOR_WGPU
    releaseImGuiTexture(entry.texture);
#else
    if (entry.texture != 0 && entry.glTextureLifetime == nullptr) {
      glDeleteTextures(1, &entry.texture);
    }
#endif
  }
  prepared.newEntries.clear();
}

const GlTextureCache::CachedTextureEntry* GlTextureCache::reusableEntry(
    const RenderResult::CompositedTile& tile, const TextureEntries& entries) {
  const auto found = entries.find(tile.id);
  return found != entries.end() &&
                 TextureIdentityMatchesCompositedTile(found->second.identity, tile)
             ? &found->second
             : nullptr;
}

std::optional<GlTextureCache::CachedTextureEntry> GlTextureCache::prepareTileEntry(
    const RenderResult::CompositedTile& tile, const TextureEntries& prior,
    const TextureEntries* fallback, PreparedTileSet& prepared) {
  const CachedTextureEntry* reusable = reusableEntry(tile, prior);
  if (reusable == nullptr && fallback != nullptr) {
    reusable = reusableEntry(tile, *fallback);
  }
  if (reusable != nullptr && reusable->texture != 0) {
    return *reusable;
  }
  std::optional<RenderResult::CompositedTile> retainedPayload;
#ifdef DONNER_EDITOR_WGPU
  if (reusable != nullptr && reusable->textureSnapshot != nullptr) {
    if (!HasUiTextureRegistry()) {
      return *reusable;
    }
    retainedPayload.emplace();
    retainedPayload->kind = tile.kind;
    retainedPayload->generation = tile.generation;
    retainedPayload->rasterCanvasSize = tile.rasterCanvasSize;
    retainedPayload->textureSnapshot = reusable->textureSnapshot;
  }
#endif
  auto uploaded = uploadTilePayload(retainedPayload.has_value() ? *retainedPayload : tile);
  if (!uploaded.has_value()) {
    metadataOnlyMissCount_ += !TileHasPayload(tile);
    return std::nullopt;
  }
  prepared.newEntries.push_back(*uploaded);
  return uploaded;
}

std::optional<GlTextureCache::PreparedTileSet> GlTextureCache::prepareTileSet(
    const RenderResult::CompositedPreview& preview, const TextureEntries& prior,
    const TextureEntries* fallback) {
  if (preview.tiles.empty()) {
    return std::nullopt;
  }
  PreparedTileSet prepared;
  prepared.views.reserve(preview.tiles.size());
  for (const auto& tile : preview.tiles) {
    if (tile.id.empty() || prepared.entries.contains(tile.id)) {
      discardPreparedTiles(prepared);
      return std::nullopt;
    }
    auto entry = prepareTileEntry(tile, prior, fallback, prepared);
    if (!entry.has_value()) {
      discardPreparedTiles(prepared);
      return std::nullopt;
    }
    prepared.views.push_back(makeTileView(tile, *entry));
    prepared.entries.emplace(tile.id, std::move(*entry));
  }
  return prepared;
}

bool GlTextureCache::referencedOutside(NativeTextureHandle texture,
                                       const TextureEntries& entries) const {
  const auto references = [&](const TextureEntries& candidates) {
    return &candidates != &entries && std::ranges::any_of(candidates, [&](const auto& item) {
      return item.second.texture == texture;
    });
  };
  return references(tileTextures_) || references(overviewTileTextures_);
}

void GlTextureCache::commitTileSet(PreparedTileSet prepared, TextureEntries& entries,
                                   std::vector<TileView>& views) {
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retired;
#endif
  for (const auto& [id, old] : entries) {
    const auto replacement = prepared.entries.find(id);
    if ((replacement != prepared.entries.end() && replacement->second.texture == old.texture) ||
        referencedOutside(old.texture, entries)) {
      continue;
    }
#ifdef DONNER_EDITOR_WGPU
    retired.push_back(RetiredSnapshot{
        .uiTextureLifetime = old.uiTextureLifetime,
        .texture = old.texture,
        .snapshot = old.textureSnapshot,
        .allocationDimensions = Vector2i(old.allocatedWidth, old.allocatedHeight),
    });
#else
    if (old.texture != 0 && old.glTextureLifetime == nullptr) {
      glDeleteTextures(1, &old.texture);
    }
#endif
  }
  entries = std::move(prepared.entries);
  views = std::move(prepared.views);
#ifdef DONNER_EDITOR_WGPU
  retireSnapshots(std::move(retired));
#endif
}

void GlTextureCache::recordActiveCoverage(
    const std::optional<EditorRasterViewport>& rasterViewport) {
  activeTilesViewportBounded_ = rasterViewport.has_value() && rasterViewport->viewportBounded;
  activeRasterDocumentRect_ = rasterViewport.has_value() ? rasterViewport->documentRect : Box2d();
  activeOutputSizePx_ =
      rasterViewport.has_value() ? rasterViewport->outputSizePx : Vector2i::Zero();
  duplicateLiveTextureCount_ = 0;
  std::unordered_set<ImTextureID> identities;
  for (const auto& tile : tiles_) {
    if (tile.texture != 0 && !identities.insert(tile.texture).second) {
      ++duplicateLiveTextureCount_;
    }
  }
}

std::shared_ptr<const CapturedPresentation> GlTextureCache::retainedOverviewCapture(
    bool retainAsOverview, const std::shared_ptr<const CapturedPresentation>& capture,
    const RenderResult* overview) const {
  if (retainAsOverview) {
    return capture;
  }
  if (overview != nullptr) {
    return overview->capturedPresentation;
  }
  return presentationResources_ ? presentationResources_->overviewCapture() : nullptr;
}

bool GlTextureCache::uploadComposited(const RenderResult::CompositedPreview& preview,
                                      std::optional<EditorRasterViewport> rasterViewport,
                                      const RenderResult* overview,
                                      std::shared_ptr<const CapturedPresentation> capture) {
  ZoneScopedN("GlTextureCache::uploadComposited");
  const auto uploadStart = std::chrono::steady_clock::now();
  lastCompositedUploadCost_ = CostForCompositedPreviewUpload(preview);
  metadataOnlyMissCount_ = 0;
  const bool retainAsOverview = rasterViewport.has_value() && !rasterViewport->viewportBounded;
  auto active =
      prepareTileSet(preview, tileTextures_, retainAsOverview ? &overviewTileTextures_ : nullptr);
  if (!active.has_value()) {
    return false;
  }
  std::optional<PreparedTileSet> nextOverview;
  if (retainAsOverview) {
    nextOverview = prepareTileSet(preview, overviewTileTextures_, &active->entries);
  } else if (overview != nullptr && overview->compositedPreview.has_value()) {
    nextOverview =
        prepareTileSet(*overview->compositedPreview, overviewTileTextures_, &tileTextures_);
  }
  if ((retainAsOverview || overview != nullptr) && !nextOverview.has_value()) {
    discardPreparedTiles(*active);
    return false;
  }
  commitTileSet(std::move(*active), tileTextures_, tiles_);
  if (nextOverview.has_value()) {
    commitTileSet(std::move(*nextOverview), overviewTileTextures_, overviewTiles_);
    const auto& coverage = retainAsOverview ? *rasterViewport : overview->rasterViewport;
    overviewRasterDocumentRect_ = coverage.documentRect;
    overviewOutputSizePx_ = coverage.outputSizePx;
  }
  recordActiveCoverage(rasterViewport);
  const auto overviewCapture = retainedOverviewCapture(retainAsOverview, capture, overview);
  publishResources(std::move(capture), overviewCapture);
  lastCompositedUploadCost_.uploadMs = MillisecondsSince(uploadStart);
  return true;
}

bool GlTextureCache::uploadCompositedOverview(const RenderResult::CompositedPreview& preview,
                                              const EditorRasterViewport& rasterViewport,
                                              std::shared_ptr<const CapturedPresentation> capture) {
  ZoneScopedN("GlTextureCache::uploadCompositedOverview");
  const auto uploadStart = std::chrono::steady_clock::now();
  lastCompositedUploadCost_ = CostForCompositedPreviewUpload(preview);
  metadataOnlyMissCount_ = 0;
  auto prepared = prepareTileSet(preview, overviewTileTextures_, &tileTextures_);
  if (!prepared.has_value()) {
    return false;
  }
  commitTileSet(std::move(*prepared), overviewTileTextures_, overviewTiles_);
  overviewRasterDocumentRect_ = rasterViewport.documentRect;
  overviewOutputSizePx_ = rasterViewport.outputSizePx;
  const auto activeCapture = presentationResources_ ? presentationResources_->capture() : capture;
  publishResources(activeCapture, std::move(capture));
  lastCompositedUploadCost_.uploadMs = MillisecondsSince(uploadStart);
  return true;
}

GlTextureCache::PresentationResources::CoverageIndex
GlTextureCache::PresentationResources::IndexCoverage(const CapturedPresentation& capture,
                                                     const std::vector<TileView>& tiles) {
  CoverageIndex index;
  for (const auto& tile : tiles) {
    if (tile.layerEntity == entt::null ||
        std::abs(tile.documentFromCachedDocument.determinant()) < 1e-12) {
      continue;
    }
    auto& groups = index[tile.layerEntity];
    auto group = std::ranges::find_if(groups, [&](const auto& value) {
      return SamePresentationTransform(value.documentFromRaster, tile.documentFromCachedDocument);
    });
    if (group == groups.end()) {
      RasterCoverageGroup added{.documentFromRaster = tile.documentFromCachedDocument};
      const auto rasterFromDocument = tile.documentFromCachedDocument.inverse();
      const auto object = std::ranges::find_if(
          capture.objects(), [&](const auto& value) { return value.entity == tile.layerEntity; });
      if (object != capture.objects().end() && object->pathBoundsCoverFrame) {
        for (const auto& path : object->chrome.paths) {
          added.paintBounds.push_back(path.pathDoc.transformed(rasterFromDocument).bounds());
        }
      } else {
        for (const auto& bounds : capture.paintBounds(tile.layerEntity)) {
          added.paintBounds.push_back(rasterFromDocument.transformBox(bounds));
        }
      }
      groups.push_back(std::move(added));
      group = std::prev(groups.end());
    }
    const auto origin = tile.canvasOffsetDoc + capture.documentOrigin();
    group->tileBounds.emplace_back(origin, origin + tile.bitmapDimsDoc);
  }
  return index;
}

void GlTextureCache::PresentationResources::indexCoverage() {
  if (capture_) {
    activeCoverage_ = IndexCoverage(*capture_, tiles_);
  }
  if (overviewCapture_) {
    overviewCoverage_ = IndexCoverage(*overviewCapture_, overviewTiles_);
  }
}

const std::vector<GlTextureCache::RasterCoverageGroup>&
GlTextureCache::PresentationResources::objectCoverage(Entity entity, bool overview) const {
  const auto& index = overview ? overviewCoverage_ : activeCoverage_;
  const auto found = index.find(entity);
  static const std::vector<RasterCoverageGroup> empty;
  return found == index.end() ? empty : found->second;
}

void GlTextureCache::publishResources(std::shared_ptr<const CapturedPresentation> capture,
                                      std::shared_ptr<const CapturedPresentation> overviewCapture) {
  auto resources = std::shared_ptr<PresentationResources>(new PresentationResources());
  resources->capture_ = std::move(capture);
  resources->overviewCapture_ = std::move(overviewCapture);
  resources->tiles_ = tiles_;
  resources->overviewTiles_ = overviewTiles_;
  resources->coverage_ = coverageDiagnostics();
  resources->indexCoverage();
  presentationResources_ = std::move(resources);
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

void GlTextureCache::resetComposited() {
  presentationResources_.reset();
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
  retiredSnapshots.reserve(tileTextures_.size() + overviewTileTextures_.size());
  const auto retireEntry = [&](CachedTextureEntry& entry) {
    if (entry.texture != 0) {
      retiredSnapshots.push_back(RetiredSnapshot{
          .uiTextureLifetime = entry.uiTextureLifetime,
          .texture = entry.texture,
          .snapshot = std::move(entry.textureSnapshot),
          .allocationDimensions = Vector2i(entry.allocatedWidth, entry.allocatedHeight),
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
    if (entry.texture != 0 && entry.glTextureLifetime == nullptr) {
      glDeleteTextures(1, &entry.texture);
    }
  }
  for (auto& [_, entry] : overviewTileTextures_) {
    if (entry.texture != 0 && entry.glTextureLifetime == nullptr) {
      glDeleteTextures(1, &entry.texture);
    }
  }
#endif
  tileTextures_.clear();
  overviewTileTextures_.clear();
  tiles_.clear();
  overviewTiles_.clear();
  activeTilesViewportBounded_ = false;
  activeRasterDocumentRect_ = Box2d();
  overviewRasterDocumentRect_ = Box2d();
  activeOutputSizePx_ = Vector2i::Zero();
  overviewOutputSizePx_ = Vector2i::Zero();
  metadataOnlyMissCount_ = 0;
  duplicateLiveTextureCount_ = 0;
  lastCompositedUploadCost_ = FrameCostBreakdown::CompositedUpload{};
}

GlTextureCache::ThumbnailTextureView GlTextureCache::uploadThumbnail(
    std::uint64_t key, const svg::RendererBitmap& bitmap) {
  ZoneScopedN("GlTextureCache::uploadThumbnail");
  if (bitmap.empty()) {
    return {};
  }

  // Reuse `CachedTextureEntry::uploadedGeneration` as the cached content
  // fingerprint and `width`/`height` as the cached payload dimensions so an
  // unchanged thumbnail short-circuits without touching GL/WGPU.
  const std::uint64_t fingerprint = ThumbnailBitmapFingerprint(bitmap);
  CachedTextureEntry& entry = thumbnailTextures_[key];
  const bool unchanged = entry.texture != 0 && entry.uploadedGeneration == fingerprint &&
                         entry.width == bitmap.dimensions.x && entry.height == bitmap.dimensions.y;
  if (unchanged) {
    return ThumbnailTextureView{
        .texture = ToImTextureId(entry.texture),
        .uvBottomRight = entry.uvBottomRight,
    };
  }

#ifdef DONNER_EDITOR_WGPU
  std::shared_ptr<svg::RendererGeodeTextureSnapshot> uploadedSnapshot = uploadBitmapToWgpu(bitmap);
  if (uploadedSnapshot == nullptr) {
    // The refused upload never touched the published allocation, so the preview the caller is
    // already drawing stays exactly as it was.
    return ThumbnailTextureView{
        .texture = ToImTextureId(entry.texture),
        .uvBottomRight = entry.uvBottomRight,
    };
  }
  std::shared_ptr<const void> lifetime;
  const NativeTextureHandle textureId = registerSnapshotTexture(*uploadedSnapshot, &lifetime);
  if (entry.texture != 0) {
    RetiredSnapshotBatch retiredSnapshots;
    retiredSnapshots.push_back(RetiredSnapshot{
        .uiTextureLifetime = entry.uiTextureLifetime,
        .texture = entry.texture,
        .snapshot = std::move(entry.textureSnapshot),
        .allocationDimensions = Vector2i(entry.allocatedWidth, entry.allocatedHeight),
    });
    retireSnapshots(std::move(retiredSnapshots));
  }
  const Vector2i allocationDimensions = uploadedSnapshot->allocationDimensions();
  entry.textureSnapshot = std::move(uploadedSnapshot);
  entry.uiTextureLifetime = std::move(lifetime);
  entry.texture = textureId;
  entry.width = bitmap.dimensions.x;
  entry.height = bitmap.dimensions.y;
  entry.allocatedWidth = allocationDimensions.x;
  entry.allocatedHeight = allocationDimensions.y;
  entry.uvBottomRight = TextureUvBottomRightForPayload(bitmap.dimensions, allocationDimensions);
#else
  if (entry.texture == 0) {
    glGenTextures(1, &entry.texture);
    InitializeTexture(entry.texture);
  }
  UploadBitmap(entry.texture, bitmap, &entry.width, &entry.height, &entry.allocatedWidth,
               &entry.allocatedHeight, &entry.uvBottomRight);
#endif
  entry.identity = CompositedTileTextureIdentity{};
  entry.uploadedGeneration = fingerprint;
  return ThumbnailTextureView{
      .texture = ToImTextureId(entry.texture),
      .uvBottomRight = entry.uvBottomRight,
  };
}

GlTextureCache::ThumbnailTextureView GlTextureCache::retainThumbnailTextureSnapshot(
    std::uint64_t key, std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot) {
#ifdef DONNER_EDITOR_WGPU
  CachedTextureEntry& entry = thumbnailTextures_[key];
  if (textureSnapshot == entry.textureSnapshot && entry.texture != 0) {
    return ThumbnailTextureView{
        .texture = ToImTextureId(entry.texture),
        .uvBottomRight = entry.uvBottomRight,
    };
  }

  std::shared_ptr<const void> lifetime;
  const NativeTextureHandle textureId =
      textureSnapshot != nullptr ? registerSnapshotTexture(*textureSnapshot, &lifetime) : 0;
  if (textureId == 0) {
    return {};
  }

  if (entry.texture != 0) {
    RetiredSnapshotBatch retiredSnapshots;
    retiredSnapshots.push_back(RetiredSnapshot{
        .uiTextureLifetime = entry.uiTextureLifetime,
        .texture = entry.texture,
        .snapshot = std::move(entry.textureSnapshot),
        .allocationDimensions = Vector2i(entry.allocatedWidth, entry.allocatedHeight),
    });
    retireSnapshots(std::move(retiredSnapshots));
  }

  entry.uiTextureLifetime = std::move(lifetime);
  entry.texture = textureId;
  entry.textureSnapshot = std::move(textureSnapshot);
  entry.identity = CompositedTileTextureIdentity{};
  entry.uploadedGeneration = 0;
  entry.width = entry.textureSnapshot->dimensions().x;
  entry.height = entry.textureSnapshot->dimensions().y;
  entry.allocatedWidth = entry.width;
  entry.allocatedHeight = entry.height;
  entry.uvBottomRight = Vector2d(1.0, 1.0);
  return ThumbnailTextureView{
      .texture = ToImTextureId(entry.texture),
      .uvBottomRight = entry.uvBottomRight,
  };
#else
  (void)key;
  (void)textureSnapshot;
  return {};
#endif
}

void GlTextureCache::retainThumbnailsOnly(const std::vector<std::uint64_t>& liveKeys) {
  if (thumbnailTextures_.empty()) {
    return;
  }
  const std::unordered_set<std::uint64_t> liveKeySet(liveKeys.begin(), liveKeys.end());
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
#endif
  for (auto it = thumbnailTextures_.begin(); it != thumbnailTextures_.end();) {
    if (liveKeySet.find(it->first) == liveKeySet.end()) {
#ifndef DONNER_EDITOR_WGPU
      if (it->second.texture != 0) {
        glDeleteTextures(1, &it->second.texture);
      }
#else
      if (it->second.texture != 0) {
        retiredSnapshots.push_back(RetiredSnapshot{
            .uiTextureLifetime = it->second.uiTextureLifetime,
            .texture = it->second.texture,
            .snapshot = std::move(it->second.textureSnapshot),
            .allocationDimensions = Vector2i(it->second.allocatedWidth, it->second.allocatedHeight),
        });
      }
#endif
      it = thumbnailTextures_.erase(it);
    } else {
      ++it;
    }
  }
#ifdef DONNER_EDITOR_WGPU
  retireSnapshots(std::move(retiredSnapshots));
#endif
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
    if (entry.allocatedWidth > 0 && entry.allocatedHeight > 0) {
      return allocationBytes(Vector2i(entry.allocatedWidth, entry.allocatedHeight));
    }
    if (entry.textureSnapshot != nullptr) {
      return allocationBytes(entry.textureSnapshot->dimensions());
    }
    return allocationBytes(Vector2i(entry.width, entry.height));
  };

  stats.documentCompositeBytes = documentCompositeBytes_;
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
    if (retired.allocationDimensions.x > 0 && retired.allocationDimensions.y > 0) {
      return allocationBytes(retired.allocationDimensions);
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

  stats.totalTrackedBytes = stats.documentCompositeBytes + stats.activeTileBytes +
                            stats.overviewTileBytes + stats.pendingRetiredBytes +
                            stats.agedRetiredBytes;
  peakTrackedResourceBytes_ = std::max(peakTrackedResourceBytes_, stats.totalTrackedBytes);
  stats.peakTrackedBytes = peakTrackedResourceBytes_;
  return stats;
}

PresentationCoverageDiagnostics GlTextureCache::coverageDiagnostics() const {
  return PresentationCoverageDiagnostics{
      .activeTilesViewportBounded = activeTilesViewportBounded_,
      .overviewInfillAvailable = !overviewTiles_.empty(),
      .activeRasterDocumentRect = activeRasterDocumentRect_,
      .overviewRasterDocumentRect = overviewRasterDocumentRect_,
      .activeOutputSizePx = activeOutputSizePx_,
      .overviewOutputSizePx = overviewOutputSizePx_,
  };
}

ImTextureID GlTextureCache::ToImTextureId(NativeTextureHandle texture) {
#ifdef DONNER_EDITOR_WGPU
  return texture;
#else
  return static_cast<ImTextureID>(texture);
#endif
}

#ifdef DONNER_EDITOR_WGPU
void GlTextureCache::releaseImGuiTexture(NativeTextureHandle texture) {
  registeredBackings_.erase(texture);
}

GlTextureCache::NativeTextureHandle GlTextureCache::registerSnapshotTexture(
    const svg::RendererTextureSnapshot& snapshot, std::shared_ptr<const void>* lifetime) {
  UiTextureBacking backing;
  const NativeTextureHandle handle = RegisterUiSnapshotTexture(snapshot, &backing);
  auto* renderer = CurrentImGuiRuntimeRenderer();
  if (handle == 0 || renderer == nullptr) {
    return 0;
  }
  const auto owner = renderer->retirementLifetime();
  auto lease = std::shared_ptr<UiTextureBacking>(
      new UiTextureBacking(std::move(backing)), [handle, renderer, owner](UiTextureBacking* value) {
        if (!owner.expired()) {
          const auto id = UiTextureId::FromImTextureId(handle);
          if (renderer->registry().retire(id).hasResult() &&
              CurrentImGuiRuntimeRenderer() == renderer) {
            renderer->retainTextureBackingUntilReleased(id, std::move(value->texture),
                                                        std::move(value->view));
          }
        }
        delete value;
      });
  *lifetime = lease;
  registeredBackings_[handle] = lease;
  return handle;
}

std::shared_ptr<svg::RendererGeodeTextureSnapshot> GlTextureCache::uploadBitmapToWgpu(
    const svg::RendererBitmap& bitmap) {
  return UploadRuntimeBitmap(geodeDevice_, bitmap.pixels, bitmap.dimensions, bitmap.rowBytes,
                             bitmap.alphaType,
                             PowerOfTwoTextureDimensionsForPayload(bitmap.dimensions));
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
  bool initializedAllocation = false;
  if (*outAllocatedWidth != allocationDimensions.x ||
      *outAllocatedHeight != allocationDimensions.y) {
    const std::size_t allocationRowBytes = static_cast<std::size_t>(allocationDimensions.x) * 4u;
    std::vector<uint8_t> initializedPixels(allocationRowBytes *
                                           static_cast<std::size_t>(allocationDimensions.y));
    for (int y = 0; y < allocationDimensions.y; ++y) {
      const int sourceY = std::min(y, bitmap.dimensions.y - 1);
      const uint8_t* sourceRow =
          bitmap.pixels.data() + static_cast<std::size_t>(sourceY) * bitmap.rowBytes;
      uint8_t* destinationRow =
          initializedPixels.data() + static_cast<std::size_t>(y) * allocationRowBytes;
      std::memcpy(destinationRow, sourceRow, static_cast<std::size_t>(bitmap.dimensions.x) * 4u);

      const uint8_t* edgePixel = sourceRow + static_cast<std::size_t>(bitmap.dimensions.x - 1) * 4u;
      for (int x = bitmap.dimensions.x; x < allocationDimensions.x; ++x) {
        std::memcpy(destinationRow + static_cast<std::size_t>(x) * 4u, edgePixel, 4u);
      }
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, allocationDimensions.x, allocationDimensions.y, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, initializedPixels.data());
    *outAllocatedWidth = allocationDimensions.x;
    *outAllocatedHeight = allocationDimensions.y;
    initializedAllocation = true;
  }

  if (!initializedAllocation) {
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bitmap.dimensions.x, bitmap.dimensions.y, GL_RGBA,
                    GL_UNSIGNED_BYTE, bitmap.pixels.data());
    if (allocationDimensions.y > bitmap.dimensions.y) {
      const uint8_t* bottomRow =
          bitmap.pixels.data() +
          static_cast<std::size_t>(bitmap.dimensions.y - 1) * bitmap.rowBytes;
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
