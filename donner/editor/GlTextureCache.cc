#include "donner/editor/GlTextureCache.h"

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

}  // namespace

#ifdef DONNER_EDITOR_WGPU
struct GlTextureCache::WgpuUploadedTexture {
  donner::geode::ScopedWgpuHandle<wgpu::Texture> texture;
  donner::geode::ScopedWgpuHandle<wgpu::TextureView> view;
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

GlTextureCache::~GlTextureCache() {
#ifdef DONNER_EDITOR_WGPU
  releaseImGuiTexture(overlayTexture_);
  for (const auto& [_, entry] : tileTextures_) {
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
  overlayUploadedTexture_ = uploadBitmapToWgpu(bitmap);
  overlayTexture_ = overlayUploadedTexture_ != nullptr
                        ? TextureViewToImTextureId(overlayUploadedTexture_->view.get())
                        : 0;
  if (overlayTexture_ != 0) {
    overlayWidth_ = bitmap.dimensions.x;
    overlayHeight_ = bitmap.dimensions.y;
  } else {
    overlayWidth_ = 0;
    overlayHeight_ = 0;
  }
  retireSnapshots(std::move(retiredSnapshots));
#else
  UploadBitmap(overlayTexture_, bitmap, &overlayWidth_, &overlayHeight_);
#endif
}

void GlTextureCache::uploadOverlayTexture(
    std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot) {
  ZoneScopedN("GlTextureCache::uploadOverlayTexture");
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
  } else {
    overlayTextureSnapshot_.reset();
    overlayTexture_ = 0;
    overlayWidth_ = 0;
    overlayHeight_ = 0;
  }
  retireSnapshots(std::move(retiredSnapshots));
#else
  (void)textureSnapshot;
  clearOverlay();
#endif
}

void GlTextureCache::uploadComposited(const RenderResult::CompositedPreview& preview) {
  ZoneScopedN("GlTextureCache::uploadComposited");

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
      TileView view;
      view.texture = ToImTextureId(it->second.texture);
      view.id = tile.id;
      view.kind = tile.kind;
      view.layerEntity = tile.layerEntity;
      view.generation = tile.generation;
      view.bitmapDimsPx = Vector2i(it->second.width, it->second.height);
      view.rasterCanvasSize = tile.rasterCanvasSize;
      view.canvasOffsetDoc = tile.canvasOffsetDoc;
      view.bitmapDimsDoc = tile.bitmapDimsDoc;
      view.dragTranslationDoc = tile.dragTranslationDoc;
      view.documentFromCachedDocument = tile.documentFromCachedDocument;
      view.metadataOnly = true;
      view.isDragTarget = tile.isDragTarget;
      tiles_.push_back(view);
      continue;
    }

#ifdef DONNER_EDITOR_WGPU
    auto& entry = tileTextures_[tile.id];
    const CompositedTileTextureIdentity tileIdentity = TextureIdentityForCompositedTile(tile);
    if (entry.texture == 0 || entry.identity != tileIdentity) {
      NativeTextureHandle textureId = 0;
      Vector2i textureDims = Vector2i::Zero();
      std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot;
      std::shared_ptr<WgpuUploadedTexture> uploadedTexture;

      if (tile.textureSnapshot != nullptr) {
        textureId = ToImTextureId(tile.textureSnapshot.get());
        textureDims = tile.textureSnapshot->dimensions();
        textureSnapshot = tile.textureSnapshot;
      } else if (!tile.bitmap.empty()) {
        uploadedTexture = uploadBitmapToWgpu(tile.bitmap);
        if (uploadedTexture != nullptr) {
          textureId = TextureViewToImTextureId(uploadedTexture->view.get());
          textureDims = tile.bitmap.dimensions;
        }
      }

      if (textureId == 0) {
        continue;
      }

      if (entry.texture != 0) {
        retiredSnapshots.push_back(RetiredSnapshot{
            .texture = entry.texture,
            .snapshot = std::move(entry.textureSnapshot),
            .uploadedTexture = std::move(entry.uploadedTexture),
        });
      }
      if (textureSnapshot != nullptr) {
        ImGui_ImplWGPU_AddTexturePremultipliedAlphaRef(textureId);
      }
      entry.texture = textureId;
      entry.textureSnapshot = std::move(textureSnapshot);
      entry.uploadedTexture = std::move(uploadedTexture);
      entry.identity = tileIdentity;
      entry.uploadedGeneration = tile.generation;
      entry.width = textureDims.x;
      entry.height = textureDims.y;
    }
    liveIds.insert(tile.id);
#else
    liveIds.insert(tile.id);
    auto& entry = tileTextures_[tile.id];
    if (entry.texture == 0) {
      glGenTextures(1, &entry.texture);
      InitializeTexture(entry.texture);
    }

    // Re-upload only when the tile's generation advances OR the bitmap
    // dimensions changed (e.g. canvas resize landed a fresh rasterize at
    // a new size with the same generation seed — defensive).
    const CompositedTileTextureIdentity tileIdentity = TextureIdentityForCompositedTile(tile);
    const bool needsUpload = entry.identity != tileIdentity;
    if (needsUpload) {
      UploadBitmap(entry.texture, tile.bitmap, &entry.width, &entry.height);
      entry.identity = tileIdentity;
      entry.uploadedGeneration = tile.generation;
    }
#endif

    TileView view;
    view.texture = ToImTextureId(entry.texture);
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
    view.documentFromCachedDocument = tile.documentFromCachedDocument;
    view.isDragTarget = tile.isDragTarget;
    tiles_.push_back(view);
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
}

void GlTextureCache::resetComposited() {
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
  retiredSnapshots.reserve(tileTextures_.size());
  for (auto& [_, entry] : tileTextures_) {
    if (entry.texture != 0) {
      retiredSnapshots.push_back(RetiredSnapshot{
          .texture = entry.texture,
          .snapshot = std::move(entry.textureSnapshot),
          .uploadedTexture = std::move(entry.uploadedTexture),
      });
    }
  }
  retireSnapshots(std::move(retiredSnapshots));
#else
  for (auto& [_, entry] : tileTextures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
#endif
  tileTextures_.clear();
  tiles_.clear();
  metadataOnlyMissCount_ = 0;
  duplicateLiveTextureCount_ = 0;
}

ImTextureID GlTextureCache::overlayTexture() const {
  return ToImTextureId(overlayTexture_);
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
    const svg::RendererBitmap& bitmap) {
  if (geodeDevice_ == nullptr || bitmap.empty() || bitmap.rowBytes == 0u) {
    return nullptr;
  }

  const uint32_t width = static_cast<uint32_t>(bitmap.dimensions.x);
  const uint32_t height = static_cast<uint32_t>(bitmap.dimensions.y);
  const uint32_t tightBytesPerRow = width * 4u;
  const uint32_t paddedBytesPerRow = AlignWgpuBytesPerRow(tightBytesPerRow);

  if (bitmap.rowBytes < tightBytesPerRow ||
      bitmap.pixels.size() < bitmap.rowBytes * static_cast<std::size_t>(height)) {
    return nullptr;
  }

  wgpu::TextureDescriptor textureDesc = {};
  textureDesc.label = donner::geode::wgpuLabel("EditorUploadedBitmap");
  textureDesc.size = {width, height, 1};
  textureDesc.mipLevelCount = 1;
  textureDesc.sampleCount = 1;
  textureDesc.dimension = wgpu::TextureDimension::_2D;
  textureDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  textureDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;

  auto uploaded = std::make_shared<WgpuUploadedTexture>();
  uploaded->texture.reset(geodeDevice_->device().createTexture(textureDesc));
  if (!uploaded->texture) {
    return nullptr;
  }
  geodeDevice_->countTexture();

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

  uploaded->view.reset(uploaded->texture.get().createView());
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
                                  int* outHeight) {
  if (bitmap.empty()) {
    *outWidth = 0;
    *outHeight = 0;
    return;
  }

  glBindTexture(GL_TEXTURE_2D, texture);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, bitmap.pixels.data());
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  *outWidth = bitmap.dimensions.x;
  *outHeight = bitmap.dimensions.y;
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
