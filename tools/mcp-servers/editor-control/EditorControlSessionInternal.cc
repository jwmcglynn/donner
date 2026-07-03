/// @file
/// Shared helper implementations for the EditorControlSession tool files —
/// argument parsing, JSON serialization, content hashing, and bitmap
/// summaries/attachments.

#include "tools/mcp-servers/editor-control/EditorControlSessionInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectTool.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "nlohmann/json.hpp"

namespace donner::editor::mcp {

namespace {

using nlohmann::json;

bool IsGraphicsElement(const svg::SVGElement& element) {
  return element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
    return element.isa<svg::SVGGraphicsElement>();
  });
}

json EntityListToJson(std::span<const Entity> entities) {
  json out = json::array();
  for (Entity entity : entities) {
    out.push_back(EntityToJsonValue(entity));
  }
  return out;
}

std::uint64_t HashByte(std::uint64_t hash, std::uint8_t byte) {
  static constexpr std::uint64_t kFnvPrime = 1099511628211ull;
  return (hash ^ byte) * kFnvPrime;
}

std::uint64_t HashUint64(std::uint64_t hash, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    hash = HashByte(hash, static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
  }
  return hash;
}

std::string HexHash(std::uint64_t hash) {
  std::ostringstream out;
  out << "0x" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

std::string InteractionKindName(svg::compositor::InteractionHint kind) {
  switch (kind) {
    case svg::compositor::InteractionHint::Selection: return "selection";
    case svg::compositor::InteractionHint::ActiveDrag: return "active_drag";
  }
  return "unknown";
}

std::string TileKindName(RenderResult::CompositedTile::Kind kind) {
  switch (kind) {
    case RenderResult::CompositedTile::Kind::Segment:
    // Immediate (direct-rendered) tiles are still static segments in the paint
    // stack, so they serialize as "segment" to preserve the stable split-layer
    // paint order.
    case RenderResult::CompositedTile::Kind::Immediate: return "segment";
    case RenderResult::CompositedTile::Kind::Layer: return "layer";
  }
  return "unknown";
}

json BitmapPixelSample(const svg::RendererBitmap& bitmap, int x, int y) {
  if (bitmap.empty() || x < 0 || y < 0 || x >= bitmap.dimensions.x || y >= bitmap.dimensions.y) {
    return nullptr;
  }

  const std::size_t rowBytes =
      bitmap.rowBytes > 0 ? bitmap.rowBytes : static_cast<std::size_t>(bitmap.dimensions.x) * 4u;
  const std::size_t offset =
      static_cast<std::size_t>(y) * rowBytes + static_cast<std::size_t>(x) * 4u;
  if (offset + 3u >= bitmap.pixels.size()) {
    return nullptr;
  }

  return json{
      {"x", x},
      {"y", y},
      {"r", static_cast<int>(bitmap.pixels[offset])},
      {"g", static_cast<int>(bitmap.pixels[offset + 1u])},
      {"b", static_cast<int>(bitmap.pixels[offset + 2u])},
      {"a", static_cast<int>(bitmap.pixels[offset + 3u])},
  };
}

std::optional<std::string> EncodeBitmapPngBase64(const svg::RendererBitmap& bitmap) {
  if (bitmap.empty()) {
    return std::nullopt;
  }

  const size_t strideInPixels =
      bitmap.rowBytes > 0 ? bitmap.rowBytes / 4 : static_cast<size_t>(bitmap.dimensions.x);
  std::vector<uint8_t> png = svg::RendererImageIO::writeRgbaPixelsToPngMemory(
      bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y, strideInPixels);
  if (png.empty()) {
    return std::nullopt;
  }

  return Base64Encode(png);
}

json RenderResultJson(const RenderResult& result, ToolCallResult* out,
                      const EditorControlSession::CaptureOptions& capture,
                      std::string_view imagePrefix) {
  json resultJson{
      {"version", result.version},
      {"worker_ms", result.workerMs},
      {"bitmap", BitmapSummary(result.bitmap)},
  };

  if (capture.includeFinalFrame) {
    AttachBitmapImage(out, std::string(imagePrefix) + "/final_frame", result.bitmap,
                      capture.embedPngBase64, &resultJson["bitmap"]);
  }

  if (result.compositedPreview.has_value()) {
    const RenderResult::CompositedPreview& preview = *result.compositedPreview;
    json previewJson{
        {"entity", EntityToJsonValue(preview.entity)},
        {"interaction_kind", InteractionKindName(preview.interactionKind)},
        {"tile_count", preview.tiles.size()},
        {"tiles", json::array()},
    };

    int index = 0;
    for (const RenderResult::CompositedTile& tile : preview.tiles) {
      json tileJson{
          {"index", index},
          {"kind", TileKindName(tile.kind)},
          {"id", tile.id},
          {"generation", tile.generation},
          {"bitmap", BitmapSummary(tile.bitmap)},
          {"canvas_offset_doc", VectorToJson(tile.canvasOffsetDoc)},
          {"bitmap_dims_doc", VectorToJson(tile.bitmapDimsDoc)},
          {"drag_translation_doc", VectorToJson(tile.dragTranslationDoc)},
          {"document_from_cached_document", TransformToJson(tile.documentFromCachedDocument)},
          {"is_drag_target", tile.isDragTarget},
      };

      if (capture.includeTileImages && !tile.bitmap.empty()) {
        AttachBitmapImage(
            out, std::string(imagePrefix) + "/tile_" + std::to_string(index) + "_" + tile.id,
            tile.bitmap, capture.embedPngBase64, &tileJson["bitmap"]);
      }

      previewJson["tiles"].push_back(std::move(tileJson));
      ++index;
    }
    resultJson["composited_preview"] = std::move(previewJson);
  } else {
    resultJson["composited_preview"] = nullptr;
  }

  return resultJson;
}

json CapturedRenderResultJson(const EditorControlSession::CapturedRenderResult& captured,
                              ToolCallResult* out,
                              const EditorControlSession::CaptureOptions& capture,
                              std::string_view imagePrefix) {
  json resultJson = RenderResultJson(captured.renderResult, out, capture, imagePrefix);
  resultJson["display_preview"] = DisplayFrameJson(captured.displayFrame);
  return resultJson;
}

}  // namespace

Entity SelectedGraphicsEntity(EditorApp& app) {
  if (!app.selectedElement().has_value() || !IsGraphicsElement(*app.selectedElement())) {
    return entt::null;
  }

  return app.selectedElement()->unsafeEntityHandle().entity();
}

std::optional<PresentedDragBaseline> PresentedBaselineFromSelectPreviews(
    const std::optional<SelectTool::ActiveDragPreview>& activePreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedPreview) {
  if (!activePreview.has_value() || !displayedPreview.has_value() ||
      activePreview->entity != displayedPreview->entity) {
    return std::nullopt;
  }

  return PresentedDragBaseline{
      .entity = activePreview->entity,
      .representedTranslationDoc = displayedPreview->translation,
      .activeTranslationDoc = activePreview->translation,
      .representedDocumentFromCachedDocument = displayedPreview->documentFromCachedDocument,
      .activeDocumentFromCachedDocument = activePreview->documentFromCachedDocument,
  };
}

PresentedFrameTileGeometry PresentedGeometryFromDisplayTile(
    const EditorControlSession::DisplayTileView& tile) {
  return PresentedFrameTileGeometry{
      .canvasOffsetDoc = tile.canvasOffsetDoc,
      .bitmapDimsDoc = tile.bitmapDimsDoc,
      .dragTranslationDoc = tile.dragTranslationDoc,
      .documentFromCachedDocument = tile.documentFromCachedDocument,
      .isDragTarget = tile.isDragTarget,
  };
}

ToolCallResult MakeErrorResult(std::string message) {
  ToolCallResult result;
  result.isError = true;
  result.body = json{
      {"ok", false},
      {"error", std::move(message)},
  };
  return result;
}

std::string Base64Encode(std::span<const uint8_t> bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t a = bytes[i];
    const uint32_t b = (i + 1 < bytes.size()) ? bytes[i + 1] : 0;
    const uint32_t c = (i + 2 < bytes.size()) ? bytes[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;

    out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 0x3f] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 0x3f] : '=');
  }
  return out;
}

bool ReadRequiredString(const json& arguments, std::string_view key, std::string* out,
                        std::string* error) {
  const auto it = arguments.find(key);
  if (it == arguments.end() || !it->is_string()) {
    *error = "missing string argument: " + std::string(key);
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

bool ReadOptionalString(const json& arguments, std::string_view key, std::string_view fallback,
                        std::string* out, std::string* error) {
  const auto it = arguments.find(key);
  if (it == arguments.end() || it->is_null()) {
    *out = std::string(fallback);
    return true;
  }
  if (!it->is_string()) {
    *error = "argument must be a string: " + std::string(key);
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

bool ReadOptionalBool(const json& arguments, std::string_view key, bool fallback, bool* out,
                      std::string* error) {
  const auto it = arguments.find(key);
  if (it == arguments.end() || it->is_null()) {
    *out = fallback;
    return true;
  }
  if (!it->is_boolean()) {
    *error = "argument must be a boolean: " + std::string(key);
    return false;
  }
  *out = it->get<bool>();
  return true;
}

bool ReadOptionalInt(const json& arguments, std::string_view key, int fallback, int* out,
                     std::string* error) {
  const auto it = arguments.find(key);
  if (it == arguments.end() || it->is_null()) {
    *out = fallback;
    return true;
  }
  if (!it->is_number_integer()) {
    *error = "argument must be an integer: " + std::string(key);
    return false;
  }
  if (it->is_number_unsigned()) {
    const uint64_t value = it->get<uint64_t>();
    if (value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
      *error = "integer argument out of range: " + std::string(key);
      return false;
    }
    *out = static_cast<int>(value);
    return true;
  }

  const int64_t value = it->get<int64_t>();
  if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
    *error = "integer argument out of range: " + std::string(key);
    return false;
  }
  *out = static_cast<int>(value);
  return true;
}

bool ReadOptionalUint64(const json& arguments, std::string_view key, std::uint64_t fallback,
                        std::uint64_t* out, std::string* error) {
  const auto it = arguments.find(key);
  if (it == arguments.end() || it->is_null()) {
    *out = fallback;
    return true;
  }
  if (!it->is_number_integer()) {
    *error = "argument must be an integer: " + std::string(key);
    return false;
  }
  if (it->is_number_unsigned()) {
    *out = it->get<std::uint64_t>();
    return true;
  }

  const int64_t value = it->get<int64_t>();
  if (value < 0) {
    *error = "integer argument must be non-negative: " + std::string(key);
    return false;
  }
  *out = static_cast<std::uint64_t>(value);
  return true;
}

bool ReadNonNegativeIntMember(const json& object, std::string_view key, int* out,
                              std::string* error) {
  int value = 0;
  if (!ReadOptionalInt(object, key, 0, &value, error)) {
    return false;
  }
  if (value < 0) {
    *error = "integer argument must be non-negative: " + std::string(key);
    return false;
  }
  *out = value;
  return true;
}

bool ReadOptionalDouble(const json& arguments, std::string_view key, double fallback, double* out,
                        std::string* error) {
  const auto it = arguments.find(key);
  if (it == arguments.end() || it->is_null()) {
    *out = fallback;
    return true;
  }
  if (!it->is_number()) {
    *error = "argument must be a number: " + std::string(key);
    return false;
  }
  *out = it->get<double>();
  return true;
}

Box2d DocumentViewBoxOr(const svg::SVGDocument& document, const Box2d& fallback) {
  return document.withReadAccess([&document, &fallback](svg::DocumentReadAccess&) {
    return document.svgElement().viewBox().value_or(fallback);
  });
}

bool ReadCaptureOptions(const json& arguments, bool includeFinalFrameDefault,
                        EditorControlSession::CaptureOptions* out, std::string* error) {
  return ReadOptionalBool(arguments, "include_final_frame", includeFinalFrameDefault,
                          &out->includeFinalFrame, error) &&
         ReadOptionalBool(arguments, "include_tile_images", false, &out->includeTileImages,
                          error) &&
         ReadOptionalBool(arguments, "include_display_frame", false, &out->includeDisplayFrame,
                          error) &&
         ReadOptionalBool(arguments, "embed_png_base64", false, &out->embedPngBase64, error);
}

bool ReadLoadOptions(const json& arguments, EditorControlSession::LoadOptions* out,
                     std::string* error) {
  return ReadOptionalInt(arguments, "canvas_width", 0, &out->canvasWidth, error) &&
         ReadOptionalInt(arguments, "canvas_height", 0, &out->canvasHeight, error) &&
         ReadOptionalDouble(arguments, "device_pixel_ratio", 1.0, &out->devicePixelRatio, error) &&
         ReadOptionalBool(arguments, "render_after_load", true, &out->renderAfterLoad, error) &&
         ReadCaptureOptions(arguments, false, &out->captureOptions, error);
}

uint32_t EntityToJsonValue(Entity entity) {
  return entity == entt::null ? 0u : static_cast<uint32_t>(entt::to_integral(entity));
}

json VectorToJson(const Vector2d& vector) {
  return json{{"x", vector.x}, {"y", vector.y}};
}

json VectorToJson(const Vector2i& vector) {
  return json{{"x", vector.x}, {"y", vector.y}};
}

json TransformToJson(const Transform2d& destinationFromSource) {
  return json{
      {"matrix", json::array({destinationFromSource.data[0], destinationFromSource.data[1],
                              destinationFromSource.data[2], destinationFromSource.data[3],
                              destinationFromSource.data[4], destinationFromSource.data[5]})},
      {"a", destinationFromSource.data[0]},
      {"b", destinationFromSource.data[1]},
      {"c", destinationFromSource.data[2]},
      {"d", destinationFromSource.data[3]},
      {"e", destinationFromSource.data[4]},
      {"f", destinationFromSource.data[5]},
      {"translation_doc", VectorToJson(destinationFromSource.translation())},
      {"determinant", destinationFromSource.determinant()},
      {"is_identity", destinationFromSource.isIdentity()},
      {"is_translation", destinationFromSource.isTranslation()},
  };
}

std::string ReproContentHash(std::string_view source) {
  static constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
  std::uint64_t hash = kFnvOffset;
  for (unsigned char byte : source) {
    hash = HashByte(hash, byte);
  }

  std::ostringstream out;
  out << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

std::optional<std::string> BitmapContentHash(const svg::RendererBitmap& bitmap) {
  if (bitmap.empty()) {
    return std::nullopt;
  }

  static constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
  std::uint64_t hash = kFnvOffset;
  hash = HashUint64(hash, static_cast<std::uint64_t>(bitmap.dimensions.x));
  hash = HashUint64(hash, static_cast<std::uint64_t>(bitmap.dimensions.y));
  hash = HashUint64(hash, static_cast<std::uint64_t>(bitmap.rowBytes));
  hash = HashUint64(hash, static_cast<std::uint64_t>(bitmap.alphaType));
  for (std::uint8_t byte : bitmap.pixels) {
    hash = HashByte(hash, byte);
  }
  return HexHash(hash);
}

json BoxToJson(const Box2d& box) {
  return json{
      {"top_left", VectorToJson(box.topLeft)},
      {"bottom_right", VectorToJson(box.bottomRight)},
      {"size", VectorToJson(box.size())},
      {"center", VectorToJson((box.topLeft + box.bottomRight) * 0.5)},
  };
}

json BitmapSummary(const svg::RendererBitmap& bitmap) {
  const std::optional<std::string> contentHash = BitmapContentHash(bitmap);
  json summary{
      {"dimensions", VectorToJson(bitmap.dimensions)},
      {"row_bytes", bitmap.rowBytes},
      {"pixel_bytes", bitmap.pixels.size()},
      {"empty", bitmap.empty()},
  };
  summary["content_hash"] = contentHash.has_value() ? json(*contentHash) : json(nullptr);
  if (!bitmap.empty() && bitmap.dimensions.x > 0 && bitmap.dimensions.y > 0) {
    std::size_t transparentPixels = 0;
    std::size_t opaquePixels = 0;
    std::size_t translucentPixels = 0;
    const std::size_t rowBytes =
        bitmap.rowBytes > 0 ? bitmap.rowBytes : static_cast<std::size_t>(bitmap.dimensions.x) * 4u;
    for (int y = 0; y < bitmap.dimensions.y; ++y) {
      for (int x = 0; x < bitmap.dimensions.x; ++x) {
        const std::size_t offset =
            static_cast<std::size_t>(y) * rowBytes + static_cast<std::size_t>(x) * 4u;
        if (offset + 3u >= bitmap.pixels.size()) {
          continue;
        }
        const std::uint8_t alpha = bitmap.pixels[offset + 3u];
        if (alpha == 0) {
          ++transparentPixels;
        } else if (alpha == 255) {
          ++opaquePixels;
        } else {
          ++translucentPixels;
        }
      }
    }
    summary["alpha"] = json{
        {"transparent_pixels", transparentPixels},
        {"opaque_pixels", opaquePixels},
        {"translucent_pixels", translucentPixels},
        {"center_sample",
         BitmapPixelSample(bitmap, bitmap.dimensions.x / 2, bitmap.dimensions.y / 2)},
        {"corner_samples",
         {{"top_left", BitmapPixelSample(bitmap, 0, 0)},
          {"top_right", BitmapPixelSample(bitmap, bitmap.dimensions.x - 1, 0)},
          {"bottom_left", BitmapPixelSample(bitmap, 0, bitmap.dimensions.y - 1)},
          {"bottom_right",
           BitmapPixelSample(bitmap, bitmap.dimensions.x - 1, bitmap.dimensions.y - 1)}}},
    };
  } else {
    summary["alpha"] = nullptr;
  }
  return summary;
}

std::size_t BitmapRowBytes(const svg::RendererBitmap& bitmap) {
  return bitmap.rowBytes > 0 ? bitmap.rowBytes : static_cast<std::size_t>(bitmap.dimensions.x) * 4u;
}

int AttachBitmapImage(ToolCallResult* out, const std::string& label,
                      const svg::RendererBitmap& bitmap, bool embedBase64, json* metadata) {
  std::optional<std::string> base64 = EncodeBitmapPngBase64(bitmap);
  if (!base64.has_value()) {
    (*metadata)["png_attached"] = false;
    return -1;
  }

  const int imageIndex = static_cast<int>(out->images.size());
  out->images.push_back(EncodedImage{
      .label = label,
      .mimeType = "image/png",
      .dataBase64 = *base64,
  });
  (*metadata)["png_attached"] = true;
  (*metadata)["image_index"] = imageIndex;
  if (embedBase64) {
    (*metadata)["png_base64"] = *base64;
  }
  return imageIndex;
}

json DisplayFrameJson(const EditorControlSession::DisplayFrameSnapshot& display) {
  json displayJson{
      {"path", display.path},
      {"has_cached_tiles", display.hasCachedTiles},
      {"cached_entity", EntityToJsonValue(display.cachedEntity)},
      {"displayed_entity", EntityToJsonValue(display.displayedEntity)},
      {"has_active_drag_preview", display.hasActiveDragPreview},
      {"tile_count", display.tiles.size()},
      {"tiles", json::array()},
  };

  if (display.activeDragPreview.has_value()) {
    displayJson["active_drag_preview"] = json{
        {"entity", EntityToJsonValue(display.activeDragPreview->entity)},
        {"extra_entities", EntityListToJson(display.activeDragPreview->extraEntities)},
        {"translation_doc", VectorToJson(display.activeDragPreview->translation)},
        {"document_from_cached_document",
         TransformToJson(display.activeDragPreview->documentFromCachedDocument)},
        {"drag_generation", display.activeDragPreview->dragGeneration},
    };
  } else {
    displayJson["active_drag_preview"] = nullptr;
  }

  if (display.displayedDragPreview.has_value()) {
    displayJson["displayed_drag_preview"] = json{
        {"entity", EntityToJsonValue(display.displayedDragPreview->entity)},
        {"extra_entities", EntityListToJson(display.displayedDragPreview->extraEntities)},
        {"translation_doc", VectorToJson(display.displayedDragPreview->translation)},
        {"document_from_cached_document",
         TransformToJson(display.displayedDragPreview->documentFromCachedDocument)},
        {"drag_generation", display.displayedDragPreview->dragGeneration},
    };
  } else {
    displayJson["displayed_drag_preview"] = nullptr;
  }

  int index = 0;
  const std::optional<PresentedDragBaseline> dragBaseline =
      PresentedBaselineFromSelectPreviews(display.activeDragPreview, display.displayedDragPreview);
  for (const EditorControlSession::DisplayTileView& tile : display.tiles) {
    const Vector2d effectiveDragTranslationDoc =
        ResolvePresentedTileDragTranslation(PresentedGeometryFromDisplayTile(tile), dragBaseline);
    const Transform2d effectiveDocumentFromCachedDocument =
        ResolvePresentedTileDocumentTransform(PresentedGeometryFromDisplayTile(tile), dragBaseline);
    displayJson["tiles"].push_back(json{
        {"index", index},
        {"kind", TileKindName(tile.kind)},
        {"id", tile.id},
        {"generation", tile.generation},
        {"bitmap_dims_px", VectorToJson(tile.bitmapDimsPx)},
        {"canvas_offset_doc", VectorToJson(tile.canvasOffsetDoc)},
        {"bitmap_dims_doc", VectorToJson(tile.bitmapDimsDoc)},
        {"drag_translation_doc", VectorToJson(tile.dragTranslationDoc)},
        {"effective_drag_translation_doc", VectorToJson(effectiveDragTranslationDoc)},
        {"document_from_cached_document", TransformToJson(tile.documentFromCachedDocument)},
        {"effective_document_from_cached_document",
         TransformToJson(effectiveDocumentFromCachedDocument)},
        {"is_drag_target", tile.isDragTarget},
        {"reused_previous_texture", tile.reusedPreviousTexture},
        {"content_hash", tile.contentHash.empty() ? json(nullptr) : json(tile.contentHash)},
    });
    ++index;
  }

  return displayJson;
}

json RenderResultsJson(const std::vector<EditorControlSession::CapturedRenderResult>& results,
                       ToolCallResult* out, const EditorControlSession::CaptureOptions& capture,
                       std::string_view imagePrefix) {
  json stages = json::array();
  int index = 0;
  for (const EditorControlSession::CapturedRenderResult& result : results) {
    stages.push_back(CapturedRenderResultJson(
        result, out, capture, std::string(imagePrefix) + "/stage_" + std::to_string(index)));
    ++index;
  }
  return stages;
}

}  // namespace donner::editor::mcp
