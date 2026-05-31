#include "tools/mcp-servers/editor-control/EditorControlSession.h"

#include <pixelmatch/pixelmatch.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/PresentationRenderScheduler.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/TextPatch.h"
#include "donner/editor/repro/GlRnrReplay.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::mcp {
namespace {

bool IsGraphicsElement(const svg::SVGElement& element) {
  return element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
    return element.isa<svg::SVGGraphicsElement>();
  });
}

Entity SelectedGraphicsEntity(EditorApp& app) {
  if (!app.selectedElement().has_value() || !IsGraphicsElement(*app.selectedElement())) {
    return entt::null;
  }

  return app.selectedElement()->unsafeEntityHandle().entity();
}

using nlohmann::json;

constexpr int kDefaultCanvasWidth = 892;
constexpr int kDefaultCanvasHeight = 512;
constexpr int kMaxDragFrames = 240;

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
  };
}

PresentedFrameTileGeometry PresentedGeometryFromDisplayTile(
    const EditorControlSession::DisplayTileView& tile) {
  return PresentedFrameTileGeometry{
      .canvasOffsetDoc = tile.canvasOffsetDoc,
      .bitmapDimsDoc = tile.bitmapDimsDoc,
      .dragTranslationDoc = tile.dragTranslationDoc,
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

bool NearlyEqual(double a, double b) {
  return std::abs(a - b) <= 1e-9;
}

void ApplyReproViewport(ViewportState* viewport, const repro::ReproViewport& reproViewport) {
  viewport->paneOrigin = Vector2d(reproViewport.paneOriginX, reproViewport.paneOriginY);
  viewport->paneSize = Vector2d(reproViewport.paneSizeW, reproViewport.paneSizeH);
  viewport->devicePixelRatio = reproViewport.devicePixelRatio;
  viewport->zoom = reproViewport.zoom;
  viewport->panDocPoint = Vector2d(reproViewport.panDocX, reproViewport.panDocY);
  viewport->panScreenPoint = Vector2d(reproViewport.panScreenX, reproViewport.panScreenY);
  viewport->documentViewBox = Box2d::FromXYWH(reproViewport.viewBoxX, reproViewport.viewBoxY,
                                              reproViewport.viewBoxW, reproViewport.viewBoxH);
}

bool ReproViewportMatches(const ViewportState& viewport,
                          const repro::ReproViewport& reproViewport) {
  return NearlyEqual(viewport.paneOrigin.x, reproViewport.paneOriginX) &&
         NearlyEqual(viewport.paneOrigin.y, reproViewport.paneOriginY) &&
         NearlyEqual(viewport.paneSize.x, reproViewport.paneSizeW) &&
         NearlyEqual(viewport.paneSize.y, reproViewport.paneSizeH) &&
         NearlyEqual(viewport.devicePixelRatio, reproViewport.devicePixelRatio) &&
         NearlyEqual(viewport.zoom, reproViewport.zoom) &&
         NearlyEqual(viewport.panDocPoint.x, reproViewport.panDocX) &&
         NearlyEqual(viewport.panDocPoint.y, reproViewport.panDocY) &&
         NearlyEqual(viewport.panScreenPoint.x, reproViewport.panScreenX) &&
         NearlyEqual(viewport.panScreenPoint.y, reproViewport.panScreenY) &&
         NearlyEqual(viewport.documentViewBox.topLeft.x, reproViewport.viewBoxX) &&
         NearlyEqual(viewport.documentViewBox.topLeft.y, reproViewport.viewBoxY) &&
         NearlyEqual(viewport.documentViewBox.size().x, reproViewport.viewBoxW) &&
         NearlyEqual(viewport.documentViewBox.size().y, reproViewport.viewBoxH);
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

std::uint8_t PremultiplyChannel(std::uint8_t channel, std::uint8_t alpha,
                                svg::AlphaType alphaType) {
  if (alphaType == svg::AlphaType::Premultiplied) {
    return channel;
  }
  return static_cast<std::uint8_t>((static_cast<int>(channel) * static_cast<int>(alpha) + 127) /
                                   255);
}

std::uint8_t BlendChannel(std::uint8_t src, std::uint8_t dst, std::uint8_t srcAlpha) {
  const int invAlpha = 255 - static_cast<int>(srcAlpha);
  return static_cast<std::uint8_t>(
      std::min(255, static_cast<int>(src) + (static_cast<int>(dst) * invAlpha + 127) / 255));
}

void BlendBitmapOver(svg::RendererBitmap* destination, const svg::RendererBitmap& source,
                     int targetX, int targetY, int targetWidth, int targetHeight) {
  if (destination == nullptr || destination->empty() || source.empty() || targetWidth <= 0 ||
      targetHeight <= 0) {
    return;
  }

  const std::size_t sourceRowBytes =
      source.rowBytes > 0 ? source.rowBytes : static_cast<std::size_t>(source.dimensions.x) * 4;
  const std::size_t destinationRowBytes = destination->rowBytes;
  for (int y = 0; y < targetHeight; ++y) {
    const int dy = targetY + y;
    if (dy < 0 || dy >= destination->dimensions.y) {
      continue;
    }
    const int sy =
        std::clamp(static_cast<int>((static_cast<int64_t>(y) * source.dimensions.y) / targetHeight),
                   0, source.dimensions.y - 1);
    for (int x = 0; x < targetWidth; ++x) {
      const int dx = targetX + x;
      if (dx < 0 || dx >= destination->dimensions.x) {
        continue;
      }
      const int sx = std::clamp(
          static_cast<int>((static_cast<int64_t>(x) * source.dimensions.x) / targetWidth), 0,
          source.dimensions.x - 1);
      const std::size_t sourceIndex =
          static_cast<std::size_t>(sy) * sourceRowBytes + static_cast<std::size_t>(sx) * 4;
      const std::size_t destinationIndex =
          static_cast<std::size_t>(dy) * destinationRowBytes + static_cast<std::size_t>(dx) * 4;
      if (sourceIndex + 3 >= source.pixels.size() ||
          destinationIndex + 3 >= destination->pixels.size()) {
        continue;
      }

      const std::uint8_t srcAlpha = source.pixels[sourceIndex + 3];
      const std::uint8_t srcRed =
          PremultiplyChannel(source.pixels[sourceIndex], srcAlpha, source.alphaType);
      const std::uint8_t srcGreen =
          PremultiplyChannel(source.pixels[sourceIndex + 1], srcAlpha, source.alphaType);
      const std::uint8_t srcBlue =
          PremultiplyChannel(source.pixels[sourceIndex + 2], srcAlpha, source.alphaType);
      destination->pixels[destinationIndex] =
          BlendChannel(srcRed, destination->pixels[destinationIndex], srcAlpha);
      destination->pixels[destinationIndex + 1] =
          BlendChannel(srcGreen, destination->pixels[destinationIndex + 1], srcAlpha);
      destination->pixels[destinationIndex + 2] =
          BlendChannel(srcBlue, destination->pixels[destinationIndex + 2], srcAlpha);
      destination->pixels[destinationIndex + 3] =
          BlendChannel(srcAlpha, destination->pixels[destinationIndex + 3], srcAlpha);
    }
  }
}

json BoxToJson(const Box2d& box) {
  return json{
      {"top_left", VectorToJson(box.topLeft)},
      {"bottom_right", VectorToJson(box.bottomRight)},
      {"size", VectorToJson(box.size())},
      {"center", VectorToJson((box.topLeft + box.bottomRight) * 0.5)},
  };
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
    case RenderResult::CompositedTile::Kind::Immediate: return "immediate";
  }
  return "unknown";
}

std::string CompositeTileKindName(
    svg::compositor::CompositorController::CompositeTileSnapshot::Kind kind) {
  using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
  switch (kind) {
    case Kind::Background: return "background";
    case Kind::Foreground: return "foreground";
    case Kind::Segment: return "segment";
    case Kind::Layer: return "layer";
  }
  return "unknown";
}

std::string PromoteRefusalReasonName(
    svg::compositor::CompositorController::PromoteRefusalReason reason) {
  using Reason = svg::compositor::CompositorController::PromoteRefusalReason;
  switch (reason) {
    case Reason::None: return "none";
    case Reason::InvalidEntity: return "invalid_entity";
    case Reason::LayerLimit: return "layer_limit";
    case Reason::MemoryLimit: return "memory_limit";
    case Reason::DescendantPromoted: return "descendant_promoted";
  }
  return "unknown";
}

constexpr float kDisplayDiffPixelmatchThreshold = 0.02f;
constexpr bool kDisplayDiffPixelmatchIncludeAa = false;

json BitmapSummary(const svg::RendererBitmap& bitmap) {
  const std::optional<std::string> contentHash = BitmapContentHash(bitmap);
  json summary{
      {"dimensions", VectorToJson(bitmap.dimensions)},
      {"row_bytes", bitmap.rowBytes},
      {"pixel_bytes", bitmap.pixels.size()},
      {"empty", bitmap.empty()},
  };
  summary["content_hash"] = contentHash.has_value() ? json(*contentHash) : json(nullptr);
  return summary;
}

std::size_t BitmapRowBytes(const svg::RendererBitmap& bitmap) {
  return bitmap.rowBytes > 0 ? bitmap.rowBytes : static_cast<std::size_t>(bitmap.dimensions.x) * 4u;
}

bool CanPixelmatchBitmaps(const svg::RendererBitmap& actual, const svg::RendererBitmap& expected) {
  if (actual.dimensions != expected.dimensions || actual.dimensions.x <= 0 ||
      actual.dimensions.y <= 0) {
    return false;
  }

  const std::size_t actualRowBytes = BitmapRowBytes(actual);
  const std::size_t expectedRowBytes = BitmapRowBytes(expected);
  return actualRowBytes == expectedRowBytes && actualRowBytes % 4u == 0u &&
         actual.pixels.size() == actualRowBytes * static_cast<std::size_t>(actual.dimensions.y) &&
         expected.pixels.size() ==
             expectedRowBytes * static_cast<std::size_t>(expected.dimensions.y);
}

json BitmapDiffSummary(const std::optional<svg::RendererBitmap>& actual,
                       const svg::RendererBitmap& expected) {
  if (!actual.has_value() || actual->empty() || expected.empty()) {
    return json{
        {"available", false},
        {"actual_bitmap", actual.has_value() ? BitmapSummary(*actual) : json(nullptr)},
        {"expected_bitmap", BitmapSummary(expected)},
    };
  }

  const svg::RendererBitmap& actualBitmap = *actual;
  if (!CanPixelmatchBitmaps(actualBitmap, expected)) {
    return json{
        {"available", false},
        {"actual_bitmap", BitmapSummary(actualBitmap)},
        {"expected_bitmap", BitmapSummary(expected)},
        {"dimension_mismatch", actualBitmap.dimensions != expected.dimensions},
        {"comparison", "pixelmatch"},
        {"pixelmatch_threshold", kDisplayDiffPixelmatchThreshold},
        {"pixelmatch_include_anti_aliasing", kDisplayDiffPixelmatchIncludeAa},
        {"pixelmatch_error", "bitmap layout is not compatible with pixelmatch"},
    };
  }

  std::vector<std::uint8_t> diffImage(actualBitmap.pixels.size(), 0u);
  pixelmatch::Options options;
  options.threshold = kDisplayDiffPixelmatchThreshold;
  options.includeAA = kDisplayDiffPixelmatchIncludeAa;
  const std::size_t rowBytes = BitmapRowBytes(actualBitmap);
  const int mismatchedPixels = pixelmatch::pixelmatch(
      expected.pixels, actualBitmap.pixels, diffImage, actualBitmap.dimensions.x,
      actualBitmap.dimensions.y, rowBytes / 4u, options);

  return json{
      {"available", true},
      {"actual_bitmap", BitmapSummary(actualBitmap)},
      {"expected_bitmap", BitmapSummary(expected)},
      {"dimension_mismatch", false},
      {"compared_pixels", static_cast<std::uint64_t>(actualBitmap.dimensions.x) *
                              static_cast<std::uint64_t>(actualBitmap.dimensions.y)},
      {"comparison", "pixelmatch"},
      {"pixelmatch_threshold", kDisplayDiffPixelmatchThreshold},
      {"pixelmatch_include_anti_aliasing", kDisplayDiffPixelmatchIncludeAa},
      {"differing_pixels", mismatchedPixels},
  };
}

std::filesystem::path DiagnosticOutputDir() {
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr) {
    return std::filesystem::path(dir);
  }
  return std::filesystem::temp_directory_path();
}

std::string SanitizeArtifactLabel(std::string_view label) {
  std::string out;
  out.reserve(label.size());
  for (const char c : label) {
    out.push_back(c == '/' || c == '\\' || c == ':' || c == ' ' ? '_' : c);
  }
  return out;
}

bool WriteBitmapPng(const svg::RendererBitmap& bitmap, const std::filesystem::path& path) {
  if (bitmap.empty() || bitmap.rowBytes % 4u != 0u ||
      bitmap.rowBytes != static_cast<std::size_t>(bitmap.dimensions.x) * 4u) {
    return false;
  }

  return svg::RendererImageIO::writeRgbaPixelsToPngFile(path.string().c_str(), bitmap.pixels,
                                                        bitmap.dimensions.x, bitmap.dimensions.y,
                                                        bitmap.rowBytes / 4u);
}

svg::RendererBitmap BuildDiffBitmap(const svg::RendererBitmap& actual,
                                    const svg::RendererBitmap& expected) {
  svg::RendererBitmap diff;
  if (!CanPixelmatchBitmaps(actual, expected)) {
    return diff;
  }

  const std::size_t rowBytes = BitmapRowBytes(actual);
  diff.dimensions = actual.dimensions;
  diff.rowBytes = rowBytes;
  diff.alphaType = svg::AlphaType::Premultiplied;
  diff.pixels.resize(static_cast<std::size_t>(diff.dimensions.y) * diff.rowBytes, 0u);

  pixelmatch::Options options;
  options.threshold = kDisplayDiffPixelmatchThreshold;
  options.includeAA = kDisplayDiffPixelmatchIncludeAa;
  static_cast<void>(pixelmatch::pixelmatch(expected.pixels, actual.pixels, diff.pixels,
                                           actual.dimensions.x, actual.dimensions.y, rowBytes / 4u,
                                           options));
  return diff;
}

void AddBitmapDiffArtifacts(json* diff, const std::optional<svg::RendererBitmap>& actual,
                            const svg::RendererBitmap& expected, std::string_view label) {
  if (!actual.has_value() || actual->empty() || expected.empty() ||
      !diff->value("available", false)) {
    return;
  }
  if (diff->value("differing_pixels", 0) == 0) {
    return;
  }

  const std::filesystem::path outDir = DiagnosticOutputDir();
  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  const std::string artifactLabel = SanitizeArtifactLabel(label);
  const std::filesystem::path actualPath = outDir / ("actual_" + artifactLabel + ".png");
  const std::filesystem::path expectedPath = outDir / ("expected_" + artifactLabel + ".png");
  const std::filesystem::path diffPath = outDir / ("diff_" + artifactLabel + ".png");
  const svg::RendererBitmap diffBitmap = BuildDiffBitmap(*actual, expected);

  (*diff)["artifacts"] = json{
      {"actual", WriteBitmapPng(*actual, actualPath) ? json(actualPath.string()) : json(nullptr)},
      {"expected",
       WriteBitmapPng(expected, expectedPath) ? json(expectedPath.string()) : json(nullptr)},
      {"diff", WriteBitmapPng(diffBitmap, diffPath) ? json(diffPath.string()) : json(nullptr)},
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

std::optional<std::vector<uint8_t>> ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }

  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>());
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }

  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

int AttachPngFile(ToolCallResult* out, const std::string& label, const std::filesystem::path& path,
                  bool embedBase64, json* metadata) {
  std::optional<std::vector<uint8_t>> bytes = ReadBinaryFile(path);
  if (!bytes.has_value()) {
    (*metadata)["png_attached"] = false;
    return -1;
  }

  const std::string base64 = Base64Encode(*bytes);
  const int imageIndex = static_cast<int>(out->images.size());
  out->images.push_back(EncodedImage{
      .label = label,
      .mimeType = "image/png",
      .dataBase64 = base64,
  });
  (*metadata)["png_attached"] = true;
  (*metadata)["image_index"] = imageIndex;
  if (embedBase64) {
    (*metadata)["png_base64"] = base64;
  }
  return imageIndex;
}

json ElementJson(const svg::SVGElement& element) {
  const Entity entity = element.unsafeEntityHandle().entity();
  return element.withReadAccess([&element, entity](svg::DocumentReadAccess&, EntityHandle) {
    return json{
        {"entity", EntityToJsonValue(entity)},
        {"tag", element.tagName().toString()},
        {"id", std::string(element.id())},
        {"class", std::string(element.className())},
    };
  });
}

bool DrainWritebackAndReparseSource(EditorApp* app, SelectTool* selectTool, std::string* source) {
  std::optional<SelectTool::CompletedDragWriteback> completed =
      selectTool->consumeCompletedDragWriteback();
  if (!completed.has_value()) {
    return false;
  }

  std::vector<TextPatch> patches;
  patches.reserve(1u + completed->extras.size());

  const auto appendPatchForTarget = [&](const AttributeWritebackTarget& target,
                                        const Transform2d& transform) {
    const RcString serialized = toSVGTransformString(transform);
    std::optional<TextPatch> patch;
    if (std::string_view(serialized).empty()) {
      patch = buildAttributeRemoveWriteback(*source, target, "transform");
    } else {
      patch = buildAttributeWriteback(*source, target, "transform", std::string_view(serialized));
    }
    if (patch.has_value()) {
      patches.push_back(*patch);
    }
  };

  appendPatchForTarget(completed->target, completed->transform);
  for (const SelectTool::CompletedDragWriteback& extra : completed->extras) {
    appendPatchForTarget(extra.target, extra.transform);
  }
  if (patches.empty()) {
    return false;
  }

  applyPatches(*source, patches);
  app->applyMutation(EditorCommand::ReplaceDocumentCommand(*source,
                                                           /*preserveUndoOnReparse=*/true));
  return true;
}

std::string_view ReproEventKindName(repro::ReproEvent::Kind kind) {
  switch (kind) {
    case repro::ReproEvent::Kind::MouseDown: return "mdown";
    case repro::ReproEvent::Kind::MouseUp: return "mup";
    case repro::ReproEvent::Kind::KeyDown: return "kdown";
    case repro::ReproEvent::Kind::KeyUp: return "kup";
    case repro::ReproEvent::Kind::Char: return "char";
    case repro::ReproEvent::Kind::Wheel: return "wheel";
    case repro::ReproEvent::Kind::Resize: return "resize";
    case repro::ReproEvent::Kind::Focus: return "focus";
  }

  return "unknown";
}

json ReproEventsJson(std::span<const repro::ReproEvent> events) {
  json out = json::array();
  for (const repro::ReproEvent& event : events) {
    json eventJson{
        {"kind", ReproEventKindName(event.kind)},
        {"mouse_button", event.mouseButton},
        {"key", event.key},
        {"modifiers", event.modifiers},
    };
    if (event.hit.has_value()) {
      eventJson["hit"] = json{
          {"id", event.hit->id},
          {"tag", event.hit->tag},
          {"doc_order_index", event.hit->docOrderIndex},
          {"empty", event.hit->empty},
      };
    } else {
      eventJson["hit"] = nullptr;
    }
    out.push_back(std::move(eventJson));
  }

  return out;
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
        {"translation_doc", VectorToJson(display.activeDragPreview->translation)},
    };
  } else {
    displayJson["active_drag_preview"] = nullptr;
  }

  if (display.displayedDragPreview.has_value()) {
    displayJson["displayed_drag_preview"] = json{
        {"entity", EntityToJsonValue(display.displayedDragPreview->entity)},
        {"translation_doc", VectorToJson(display.displayedDragPreview->translation)},
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
        {"is_drag_target", tile.isDragTarget},
        {"reused_previous_texture", tile.reusedPreviousTexture},
        {"content_hash", tile.contentHash.empty() ? json(nullptr) : json(tile.contentHash)},
    });
    ++index;
  }

  return displayJson;
}

json CapturedRenderResultJson(const EditorControlSession::CapturedRenderResult& captured,
                              ToolCallResult* out,
                              const EditorControlSession::CaptureOptions& capture,
                              std::string_view imagePrefix) {
  json resultJson = RenderResultJson(captured.renderResult, out, capture, imagePrefix);
  resultJson["display_preview"] = DisplayFrameJson(captured.displayFrame);
  return resultJson;
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

json CompositeTileSnapshotJson(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile, int index) {
  return json{
      {"index", index},
      {"kind", CompositeTileKindName(tile.kind)},
      {"id", tile.id},
      {"label", tile.label},
      {"bitmap_dims", VectorToJson(tile.bitmapDims)},
      {"generation", tile.generation},
      {"last_rasterize_ms", tile.lastRasterizeMs},
      {"has_valid_bitmap", tile.hasValidBitmap},
      {"is_drag_target", tile.isDragTarget},
      {"thumbnail_dims", VectorToJson(tile.thumbnailDims)},
      {"thumbnail_bytes", tile.thumbnailPixels.size()},
  };
}

}  // namespace

EditorControlSession::EditorControlSession() : selectTool_(std::make_unique<SelectTool>()) {}

EditorControlSession::~EditorControlSession() = default;

void EditorControlSession::HeadlessTextureCache::reset() {
  tileTextures_.clear();
  tiles_.clear();
}

void EditorControlSession::HeadlessTextureCache::uploadComposited(
    const RenderResult::CompositedPreview& preview) {
  std::unordered_set<std::string> liveIds;
  liveIds.reserve(preview.tiles.size());

  tiles_.clear();
  tiles_.reserve(preview.tiles.size());

  for (const RenderResult::CompositedTile& tile : preview.tiles) {
    const bool reusingExistingTexture = tile.bitmap.empty();
    if (reusingExistingTexture) {
      const auto cachedIt = tileTextures_.find(tile.id);
      if (cachedIt == tileTextures_.end()) {
        continue;
      }

      liveIds.insert(tile.id);
      DisplayTileView view;
      view.kind = cachedIt->second.kind;
      view.id = tile.id;
      view.generation = cachedIt->second.generation;
      view.bitmapDimsPx = cachedIt->second.bitmapDimsPx;
      view.canvasOffsetDoc = tile.canvasOffsetDoc;
      view.bitmapDimsDoc = tile.bitmapDimsDoc;
      view.dragTranslationDoc = tile.dragTranslationDoc;
      view.isDragTarget = tile.isDragTarget;
      view.reusedPreviousTexture = true;
      view.contentHash = cachedIt->second.contentHash;
      tiles_.push_back(std::move(view));
      continue;
    }

    liveIds.insert(tile.id);

    CachedTextureEntry& entry = tileTextures_[tile.id];
    const bool needsUpload = entry.bitmap.empty() || entry.generation != tile.generation ||
                             entry.bitmapDimsPx != tile.bitmap.dimensions;
    if (needsUpload) {
      entry.generation = tile.generation;
      entry.bitmapDimsPx = tile.bitmap.dimensions;
      entry.contentHash = BitmapContentHash(tile.bitmap).value_or("");
      entry.bitmap = tile.bitmap;
    }
    entry.kind = tile.kind;

    DisplayTileView view;
    view.kind = tile.kind;
    view.id = tile.id;
    view.generation = tile.generation;
    view.bitmapDimsPx = tile.bitmap.dimensions;
    view.canvasOffsetDoc = tile.canvasOffsetDoc;
    view.bitmapDimsDoc = tile.bitmapDimsDoc;
    view.dragTranslationDoc = tile.dragTranslationDoc;
    view.isDragTarget = tile.isDragTarget;
    view.reusedPreviousTexture = !needsUpload;
    view.contentHash = entry.contentHash;
    tiles_.push_back(std::move(view));
  }

  for (auto it = tileTextures_.begin(); it != tileTextures_.end();) {
    if (liveIds.find(it->first) == liveIds.end()) {
      it = tileTextures_.erase(it);
    } else {
      ++it;
    }
  }
}

std::optional<svg::RendererBitmap> EditorControlSession::HeadlessTextureCache::composeDisplayFrame(
    const DisplayFrameSnapshot& display, const Box2d& viewBox, const Vector2i& canvasSize) const {
  if (display.path != "tiles" || canvasSize.x <= 0 || canvasSize.y <= 0 ||
      viewBox.size().x <= 0.0 || viewBox.size().y <= 0.0) {
    return std::nullopt;
  }

  svg::RendererBitmap composed;
  composed.dimensions = canvasSize;
  composed.rowBytes = static_cast<std::size_t>(canvasSize.x) * 4;
  composed.alphaType = svg::AlphaType::Premultiplied;
  composed.pixels.resize(static_cast<std::size_t>(canvasSize.y) * composed.rowBytes, 0);

  const double pixelsPerDocX = static_cast<double>(canvasSize.x) / viewBox.size().x;
  const double pixelsPerDocY = static_cast<double>(canvasSize.y) / viewBox.size().y;
  const Transform2d canvasPixelsFromCanvasTransform =
      Transform2d::Translate(-viewBox.topLeft) *
      Transform2d::Scale(Vector2d(pixelsPerDocX, pixelsPerDocY));
  const std::optional<PresentedDragBaseline> dragBaseline =
      PresentedBaselineFromSelectPreviews(display.activeDragPreview, display.displayedDragPreview);
  for (const DisplayTileView& tile : display.tiles) {
    const auto tileIt = tileTextures_.find(tile.id);
    if (tileIt == tileTextures_.end() || tileIt->second.bitmap.empty()) {
      continue;
    }

    const std::optional<PresentedTileRect> tileRect = ComputePresentedTileRect(
        PresentedGeometryFromDisplayTile(tile), canvasPixelsFromCanvasTransform, dragBaseline);
    if (!tileRect.has_value()) {
      continue;
    }
    const std::optional<PresentedPixelRect> pixelRect =
        RoundPresentedTileRectToPixelRect(*tileRect);
    if (!pixelRect.has_value()) {
      continue;
    }

    BlendBitmapOver(&composed, tileIt->second.bitmap, pixelRect->x, pixelRect->y, pixelRect->width,
                    pixelRect->height);
  }

  return composed;
}

json EditorControlSession::toolList() {
  return json::array({
      {
          {"name", "load_document"},
          {"description", "Load an SVG file into the headless Donner editor session."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"svg_path", {{"type", "string"}, {"description", "Path to an SVG file."}}},
                    {"canvas_width", {{"type", "integer"}, {"minimum", 1}}},
                    {"canvas_height", {{"type", "integer"}, {"minimum", 1}}},
                    {"device_pixel_ratio", {{"type", "number"}, {"default", 1.0}}},
                    {"render_after_load", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"svg_path"})},
           }},
      },
      {
          {"name", "load_svg"},
          {"description", "Load SVG source bytes directly into the headless editor session."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"svg_source", {{"type", "string"}, {"description", "Complete SVG XML."}}},
                    {"source_path", {{"type", "string"}}},
                    {"canvas_width", {{"type", "integer"}, {"minimum", 1}}},
                    {"canvas_height", {{"type", "integer"}, {"minimum", 1}}},
                    {"device_pixel_ratio", {{"type", "number"}, {"default", 1.0}}},
                    {"render_after_load", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"svg_source"})},
           }},
      },
      {
          {"name", "get_svg_source"},
          {"description",
           "Return the editable SVG source draft currently owned by the headless editor session."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"offset", {{"type", "integer"}, {"minimum", 0}, {"default", 0}}},
                    {"length", {{"type", "integer"}, {"minimum", 0}}},
                }},
           }},
      },
      {
          {"name", "edit_svg_source"},
          {"description",
           "Apply incremental text edits to the editable SVG source draft, then parse and "
           "optionally render the result when the draft is valid."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"replace_source",
                     {{"type", "string"},
                      {"description", "Optional complete replacement for the SVG draft."}}},
                    {"source_path", {{"type", "string"}}},
                    {"expected_source_revision",
                     {{"type", "integer"},
                      {"minimum", 0},
                      {"description",
                       "Optional guard: reject the edit if the source draft has changed."}}},
                    {"edits",
                     {{"type", "array"},
                      {"items",
                       {{"type", "object"},
                        {"properties",
                         {
                             {"offset", {{"type", "integer"}, {"minimum", 0}}},
                             {"delete_count",
                              {{"type", "integer"}, {"minimum", 0}, {"default", 0}}},
                             {"insert", {{"type", "string"}, {"default", ""}}},
                         }},
                        {"required", json::array({"offset"})}}}}},
                    {"allow_parse_failure", {{"type", "boolean"}, {"default", true}}},
                    {"render_after_edit", {{"type", "boolean"}, {"default", true}}},
                    {"canvas_width", {{"type", "integer"}, {"minimum", 1}}},
                    {"canvas_height", {{"type", "integer"}, {"minimum", 1}}},
                    {"device_pixel_ratio", {{"type", "number"}, {"default", 1.0}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", true}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
           }},
      },
      {
          {"name", "select_by_selector"},
          {"description", "Select the first element matching a CSS selector."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"selector", {{"type", "string"}}},
                    {"render", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", false}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"selector"})},
           }},
      },
      {
          {"name", "drag_selector"},
          {"description",
           "Find an element by CSS selector and synthesize click-and-drag frames through "
           "SelectTool."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"selector", {{"type", "string"}}},
                    {"delta_x", {{"type", "number"}, {"default", 16.0}}},
                    {"delta_y", {{"type", "number"}, {"default", 0.0}}},
                    {"frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxDragFrames}}},
                    {"selection_mode",
                     {{"type", "string"},
                      {"enum", json::array({"hit_test", "direct"})},
                      {"default", "direct"}}},
                    {"render_mouse_down", {{"type", "boolean"}, {"default", true}}},
                    {"release", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", true}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"selector"})},
           }},
      },
      {
          {"name", "render_frame"},
          {"description", "Render the current editor state and return split compositor tiles."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"include_final_frame", {{"type", "boolean"}, {"default", true}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
           }},
      },
      {
          {"name", "session_state"},
          {"description", "Return selection, canvas, and compositor diagnostics."},
          {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
      },
      {
          {"name", "start_rnr_recording"},
          {"description", "Start recording subsequent MCP-driven editor gestures to an .rnr file."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"output_path", {{"type", "string"}}},
                    {"svg_path", {{"type", "string"}}},
                    {"window_width", {{"type", "integer"}, {"minimum", 1}}},
                    {"window_height", {{"type", "integer"}, {"minimum", 1}}},
                    {"display_scale", {{"type", "number"}, {"default", 1.0}}},
                    {"frame_delta_ms", {{"type", "number"}, {"default", 16.6666667}}},
                }},
           }},
      },
      {
          {"name", "stop_rnr_recording"},
          {"description", "Stop the active .rnr recording and optionally write it to disk."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"output_path", {{"type", "string"}}},
                    {"write_file", {{"type", "boolean"}, {"default", true}}},
                }},
           }},
      },
      {
          {"name", "rnr_recording_state"},
          {"description", "Return the current .rnr recording state."},
          {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
      },
      {
          {"name", "replay_rnr"},
          {"description", "Replay an .rnr file through the headless editor session."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"rnr_path", {{"type", "string"}}},
                    {"svg_path", {{"type", "string"}}},
                    {"render_each_frame", {{"type", "boolean"}, {"default", true}}},
                    {"simulate_editor_shell_frame_loop", {{"type", "boolean"}, {"default", false}}},
                    {"gl_readback", {{"type", "boolean"}, {"default", false}}},
                    {"gl_capture_frame", {{"type", "integer"}, {"minimum", 0}}},
                    {"gl_capture_left_mousedown", {{"type", "integer"}, {"minimum", 1}}},
                    {"gl_max_frame", {{"type", "integer"}, {"minimum", 0}}},
                    {"gl_crop",
                     {{"type", "string"},
                      {"enum", json::array({"full", "render-pane", "document-canvas"})},
                      {"default", "full"}}},
                    {"gl_output_dir", {{"type", "string"}}},
                    {"gl_visible", {{"type", "boolean"}, {"default", false}}},
                    {"gl_pace", {{"type", "boolean"}, {"default", true}}},
                    {"include_gl_images", {{"type", "boolean"}, {"default", true}}},
                    {"include_frame_results", {{"type", "boolean"}, {"default", true}}},
                    {"max_frame_results", {{"type", "integer"}, {"minimum", 0}}},
                    {"stop_after_mouse_ups", {{"type", "integer"}, {"minimum", 0}}},
                    {"compare_presented_after_left_mouse_down",
                     {{"type", "integer"}, {"minimum", 0}}},
                    {"compare_presented_frame_offset_after_left_mouse_down",
                     {{"type", "integer"}, {"minimum", 0}}},
                    {"include_display_diff", {{"type", "boolean"}, {"default", false}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", false}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"rnr_path"})},
           }},
      },
  });
}

ToolCallResult EditorControlSession::handleToolCall(std::string_view name, const json& arguments) {
  if (!arguments.is_object()) {
    return MakeErrorResult("tool arguments must be a JSON object");
  }
  if (name == "load_document") return loadDocument(arguments);
  if (name == "load_svg") return loadSvg(arguments);
  if (name == "get_svg_source") return getSvgSource(arguments);
  if (name == "edit_svg_source") return editSvgSource(arguments);
  if (name == "select_by_selector") return selectBySelector(arguments);
  if (name == "drag_selector") return dragSelector(arguments);
  if (name == "render_frame") return renderFrameTool(arguments);
  if (name == "session_state") return sessionState(arguments);
  if (name == "start_rnr_recording") return startRnrRecording(arguments);
  if (name == "stop_rnr_recording") return stopRnrRecording(arguments);
  if (name == "rnr_recording_state") return rnrRecordingState(arguments);
  if (name == "replay_rnr") return replayRnr(arguments);

  return MakeErrorResult("unknown tool: " + std::string(name));
}

ToolCallResult EditorControlSession::loadDocument(const json& arguments) {
  std::string error;
  std::string svgPath;
  if (!ReadRequiredString(arguments, "svg_path", &svgPath, &error)) {
    return MakeErrorResult(error);
  }

  std::ifstream file(svgPath, std::ios::binary);
  if (!file.is_open()) {
    return MakeErrorResult("failed to open SVG file: " + svgPath);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();

  LoadOptions options;
  if (!ReadLoadOptions(arguments, &options, &error)) {
    return MakeErrorResult(error);
  }

  return loadSvgSource(buffer.str(), options, svgPath);
}

ToolCallResult EditorControlSession::loadSvg(const json& arguments) {
  std::string error;
  std::string source;
  std::string sourcePath;
  LoadOptions options;
  if (!ReadRequiredString(arguments, "svg_source", &source, &error) ||
      !ReadOptionalString(arguments, "source_path", "<memory>", &sourcePath, &error) ||
      !ReadLoadOptions(arguments, &options, &error)) {
    return MakeErrorResult(error);
  }

  return loadSvgSource(source, options, sourcePath);
}

json EditorControlSession::sourceStateJson() const {
  return json{
      {"source_path", currentSourcePath_},
      {"source_revision", sourceRevision_},
      {"loaded_source_revision", loadedSourceRevision_},
      {"preview_stale", sourceRevision_ != loadedSourceRevision_},
      {"source_length", currentSourceText_.size()},
      {"source_hash", ReproContentHash(currentSourceText_)},
      {"last_parse_error", lastParseError_.has_value() ? json(*lastParseError_) : json(nullptr)},
  };
}

bool EditorControlSession::loadCurrentSourceText(const LoadOptions& options,
                                                 std::string_view sourcePath,
                                                 bool resetRenderVersion, json* loadInfo,
                                                 std::string* error) {
  if (!waitUntilIdle(error)) {
    return false;
  }

  if (!app_.loadFromString(currentSourceText_)) {
    *error = "failed to parse SVG source";
    return false;
  }

  currentSourcePath_ = std::string(sourcePath);
  app_.setCurrentFilePath(std::string(sourcePath));
  app_.setCleanSourceText(currentSourceText_);
  app_.flushFrame();

  selectTool_ = std::make_unique<SelectTool>();
  displayPresentation_ = CompositedPresentation{};
  displayTextures_.reset();
  rnrRecording_ = RnrRecordingState{};

  const Box2d viewBox = DocumentViewBoxOr(
      app_.document().document(), Box2d::FromXYWH(0, 0, kDefaultCanvasWidth, kDefaultCanvasHeight));
  canvasWidth_ = options.canvasWidth > 0
                     ? options.canvasWidth
                     : std::max(1, static_cast<int>(std::ceil(viewBox.width())));
  canvasHeight_ = options.canvasHeight > 0
                      ? options.canvasHeight
                      : std::max(1, static_cast<int>(std::ceil(viewBox.height())));
  devicePixelRatio_ = options.devicePixelRatio > 0.0 ? options.devicePixelRatio : 1.0;
  app_.document().document().setCanvasSize(canvasWidth_, canvasHeight_);

  if (resetRenderVersion) {
    nextRenderVersion_ = 1;
  }
  loadedSourceRevision_ = sourceRevision_;
  lastParseError_.reset();

  *loadInfo = json{
      {"ok", true},
      {"source_path", std::string(sourcePath)},
      {"canvas", {{"width", canvasWidth_}, {"height", canvasHeight_}}},
      {"device_pixel_ratio", devicePixelRatio_},
      {"document_view_box", BoxToJson(viewBox)},
      {"selection", selectedElementJson()},
      {"source", sourceStateJson()},
  };
  return true;
}

ToolCallResult EditorControlSession::loadSvgSource(std::string_view source,
                                                   const LoadOptions& options,
                                                   std::string_view sourcePath) {
  const std::string previousSourcePath = currentSourcePath_;
  const std::string previousSourceText = currentSourceText_;
  const std::uint64_t previousSourceRevision = sourceRevision_;
  const std::uint64_t previousLoadedSourceRevision = loadedSourceRevision_;
  const std::optional<std::string> previousParseError = lastParseError_;

  currentSourcePath_ = std::string(sourcePath);
  currentSourceText_ = std::string(source);
  ++sourceRevision_;

  ToolCallResult out;
  std::string error;
  if (!loadCurrentSourceText(options, sourcePath, /*resetRenderVersion=*/true, &out.body, &error)) {
    currentSourcePath_ = previousSourcePath;
    currentSourceText_ = previousSourceText;
    sourceRevision_ = previousSourceRevision;
    loadedSourceRevision_ = previousLoadedSourceRevision;
    lastParseError_ = previousParseError;
    return MakeErrorResult(error);
  }

  if (options.renderAfterLoad) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["render_stages"] =
        RenderResultsJson(renderResults, &out, options.captureOptions, "load");
  }
  return out;
}

ToolCallResult EditorControlSession::getSvgSource(const json& arguments) const {
  std::string error;
  int requestedOffset = 0;
  if (!ReadNonNegativeIntMember(arguments, "offset", &requestedOffset, &error)) {
    return MakeErrorResult(error);
  }

  const std::size_t offset = static_cast<std::size_t>(requestedOffset);
  if (offset > currentSourceText_.size()) {
    return MakeErrorResult("offset is past the end of the SVG source");
  }

  std::size_t length = currentSourceText_.size() - offset;
  if (arguments.contains("length") && !arguments["length"].is_null()) {
    int requestedLength = 0;
    if (!ReadNonNegativeIntMember(arguments, "length", &requestedLength, &error)) {
      return MakeErrorResult(error);
    }
    length =
        std::min(static_cast<std::size_t>(requestedLength), currentSourceText_.size() - offset);
  }

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"source", sourceStateJson()},
      {"offset", offset},
      {"length", length},
      {"text", currentSourceText_.substr(offset, length)},
  };
  return out;
}

ToolCallResult EditorControlSession::editSvgSource(const json& arguments) {
  if (rnrRecording_.active) {
    return MakeErrorResult("editing SVG source while an .rnr recording is active is not supported");
  }

  std::string error;
  (void)drainPendingWritebacks();

  bool allowParseFailure = true;
  bool renderAfterEdit = true;
  std::uint64_t expectedSourceRevision = sourceRevision_;
  std::string sourcePath;
  if (!ReadOptionalBool(arguments, "allow_parse_failure", true, &allowParseFailure, &error) ||
      !ReadOptionalBool(arguments, "render_after_edit", true, &renderAfterEdit, &error) ||
      !ReadOptionalUint64(arguments, "expected_source_revision", sourceRevision_,
                          &expectedSourceRevision, &error) ||
      !ReadOptionalString(arguments, "source_path", currentSourcePath_, &sourcePath, &error)) {
    return MakeErrorResult(error);
  }
  if (expectedSourceRevision != sourceRevision_) {
    return MakeErrorResult("expected_source_revision does not match the current SVG source");
  }

  LoadOptions options;
  options.canvasWidth = canvasWidth_;
  options.canvasHeight = canvasHeight_;
  options.devicePixelRatio = devicePixelRatio_ > 0.0 ? devicePixelRatio_ : 1.0;
  options.renderAfterLoad = renderAfterEdit;
  if (!ReadOptionalInt(arguments, "canvas_width", options.canvasWidth, &options.canvasWidth,
                       &error) ||
      !ReadOptionalInt(arguments, "canvas_height", options.canvasHeight, &options.canvasHeight,
                       &error) ||
      !ReadOptionalDouble(arguments, "device_pixel_ratio", options.devicePixelRatio,
                          &options.devicePixelRatio, &error) ||
      !ReadCaptureOptions(arguments, true, &options.captureOptions, &error)) {
    return MakeErrorResult(error);
  }

  const auto replaceIt = arguments.find("replace_source");
  const auto editsIt = arguments.find("edits");
  if ((replaceIt == arguments.end() || replaceIt->is_null()) &&
      (editsIt == arguments.end() || editsIt->is_null()) && currentSourceText_.empty()) {
    return MakeErrorResult("edit_svg_source requires replace_source or edits for an empty session");
  }

  std::string nextSource = currentSourceText_;
  if (replaceIt != arguments.end() && !replaceIt->is_null()) {
    if (!replaceIt->is_string()) {
      return MakeErrorResult("argument must be a string: replace_source");
    }
    nextSource = replaceIt->get<std::string>();
  }

  if (editsIt != arguments.end() && !editsIt->is_null()) {
    if (!editsIt->is_array()) {
      return MakeErrorResult("argument must be an array: edits");
    }
    int editIndex = 0;
    for (const json& edit : *editsIt) {
      if (!edit.is_object()) {
        return MakeErrorResult("each edit must be an object");
      }
      if (!edit.contains("offset")) {
        return MakeErrorResult("each edit requires offset");
      }

      int requestedOffset = 0;
      int requestedDeleteCount = 0;
      std::string insert;
      if (!ReadNonNegativeIntMember(edit, "offset", &requestedOffset, &error) ||
          !ReadNonNegativeIntMember(edit, "delete_count", &requestedDeleteCount, &error) ||
          !ReadOptionalString(edit, "insert", "", &insert, &error)) {
        return MakeErrorResult("edit " + std::to_string(editIndex) + ": " + error);
      }

      const std::size_t offset = static_cast<std::size_t>(requestedOffset);
      const std::size_t deleteCount = static_cast<std::size_t>(requestedDeleteCount);
      if (offset > nextSource.size()) {
        return MakeErrorResult("edit " + std::to_string(editIndex) +
                               ": offset is past the end of the SVG source");
      }
      if (deleteCount > nextSource.size() - offset) {
        return MakeErrorResult("edit " + std::to_string(editIndex) +
                               ": delete_count extends past the end of the SVG source");
      }
      nextSource.replace(offset, deleteCount, insert);
      ++editIndex;
    }
  }

  const std::string previousSourcePath = currentSourcePath_;
  const std::string previousSourceText = currentSourceText_;
  const std::uint64_t previousSourceRevision = sourceRevision_;
  const std::uint64_t previousLoadedSourceRevision = loadedSourceRevision_;
  const std::optional<std::string> previousParseError = lastParseError_;

  currentSourcePath_ = sourcePath;
  currentSourceText_ = std::move(nextSource);
  ++sourceRevision_;

  ToolCallResult out;
  json loadInfo;
  if (!loadCurrentSourceText(options, sourcePath, /*resetRenderVersion=*/true, &loadInfo, &error)) {
    lastParseError_ = error;
    if (!allowParseFailure) {
      currentSourcePath_ = previousSourcePath;
      currentSourceText_ = previousSourceText;
      sourceRevision_ = previousSourceRevision;
      loadedSourceRevision_ = previousLoadedSourceRevision;
      lastParseError_ = previousParseError;
      return MakeErrorResult(error);
    }

    out.body = json{
        {"ok", true},
        {"parsed", false},
        {"preview_stale", true},
        {"parse_error", error},
        {"source", sourceStateJson()},
        {"attached_image_count", 0},
    };
    return out;
  }

  out.body = std::move(loadInfo);
  out.body["parsed"] = true;
  out.body["preview_stale"] = false;
  if (options.renderAfterLoad) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["render_stages"] =
        RenderResultsJson(renderResults, &out, options.captureOptions, "edit_svg_source");
  }
  out.body["attached_image_count"] = out.images.size();
  return out;
}

ToolCallResult EditorControlSession::selectBySelector(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  std::string selector;
  if (!ReadRequiredString(arguments, "selector", &selector, &error)) {
    return MakeErrorResult(error);
  }
  std::optional<svg::SVGElement> element = querySelector(selector);
  if (!element.has_value()) {
    return MakeErrorResult("selector did not match an element: " + selector);
  }

  app_.setSelection(*element);
  app_.flushFrame();

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"selector", selector},
      {"selection", selectedElementJson()},
  };

  if (auto bounds = elementWorldBounds(*element); bounds.has_value()) {
    out.body["selection_bounds_doc"] = BoxToJson(*bounds);
  } else {
    out.body["selection_bounds_doc"] = nullptr;
  }

  bool shouldRender = true;
  CaptureOptions capture;
  if (!ReadOptionalBool(arguments, "render", true, &shouldRender, &error) ||
      !ReadCaptureOptions(arguments, false, &capture, &error)) {
    return MakeErrorResult(error);
  }

  if (shouldRender) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["render_stages"] =
        RenderResultsJson(renderResults, &out, capture, "select_by_selector");
  }
  return out;
}

ToolCallResult EditorControlSession::dragSelector(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  std::string selector;
  double dx = 16.0;
  double dy = 0.0;
  int requestedFrames = 8;
  std::string selectionMode;
  bool renderMouseDown = true;
  bool release = true;
  CaptureOptions capture;
  if (!ReadRequiredString(arguments, "selector", &selector, &error) ||
      !ReadOptionalDouble(arguments, "delta_x", 16.0, &dx, &error) ||
      !ReadOptionalDouble(arguments, "delta_y", 0.0, &dy, &error) ||
      !ReadOptionalInt(arguments, "frames", 8, &requestedFrames, &error) ||
      !ReadOptionalString(arguments, "selection_mode", "direct", &selectionMode, &error) ||
      !ReadOptionalBool(arguments, "render_mouse_down", true, &renderMouseDown, &error) ||
      !ReadOptionalBool(arguments, "release", true, &release, &error) ||
      !ReadCaptureOptions(arguments, true, &capture, &error)) {
    return MakeErrorResult(error);
  }
  const int frames = std::clamp(requestedFrames, 1, kMaxDragFrames);

  std::optional<svg::SVGElement> element = querySelector(selector);
  if (!element.has_value()) {
    return MakeErrorResult("selector did not match an element: " + selector);
  }

  std::optional<Box2d> bounds = elementWorldBounds(*element);
  if (!bounds.has_value() || bounds->isEmpty()) {
    return MakeErrorResult("selected element has no usable world bounds: " + selector);
  }

  const Vector2d start = (bounds->topLeft + bounds->bottomRight) * 0.5;
  const Vector2d delta(dx, dy);

  if (selectionMode == "direct") {
    app_.setSelection(*element);
    std::array<Box2d, 1> selectionBounds{*bounds};
    if (!selectTool_->tryStartRedragOnSelected(app_, start, MouseModifiers{}, selectionBounds)) {
      return MakeErrorResult("direct drag setup failed for selector: " + selector);
    }
  } else if (selectionMode == "hit_test") {
    selectTool_->onMouseDown(app_, start, MouseModifiers{});
  } else {
    return MakeErrorResult("selection_mode must be 'hit_test' or 'direct'");
  }

  if (!selectTool_->isDragging()) {
    return MakeErrorResult("mouse down did not start a drag for selector: " + selector);
  }

  if (rnrRecording_.active) {
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::MouseDown;
    event.mouseButton = 0;
    repro::ReproHit hit =
        element->withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
          return repro::ReproHit{
              .id = std::string(element->id()),
              .tag = element->tagName().toString(),
          };
        });
    event.hit = std::move(hit);
    std::vector<repro::ReproEvent> events;
    events.push_back(std::move(event));
    appendRnrFrame(start, /*mouseButtonMask=*/1, std::move(events));
  }

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"selector", selector},
      {"selection_mode", selectionMode},
      {"start_doc", VectorToJson(start)},
      {"delta_doc", VectorToJson(delta)},
      {"frames_requested", frames},
      {"selection_after_mouse_down", selectedElementJson()},
      {"element_bounds_doc", BoxToJson(*bounds)},
      {"frames", json::array()},
  };

  if (renderMouseDown) {
    app_.flushFrame();
    const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["frames"].push_back(json{
        {"label", "mouse_down"},
        {"point_doc", VectorToJson(start)},
        {"display_before_render", DisplayFrameJson(displayBeforeRender)},
        {"stages", RenderResultsJson(renderResults, &out, capture, "drag_selector/mouse_down")},
    });
  }

  for (int i = 1; i <= frames; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(frames);
    const Vector2d point = start + delta * t;
    selectTool_->onMouseMove(app_, point, /*buttonHeld=*/true);
    appendRnrFrame(point, /*mouseButtonMask=*/1, {});
    app_.flushFrame();
    const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["frames"].push_back(json{
        {"label", "move_" + std::to_string(i)},
        {"point_doc", VectorToJson(point)},
        {"display_before_render", DisplayFrameJson(displayBeforeRender)},
        {"stages", RenderResultsJson(renderResults, &out, capture,
                                     "drag_selector/move_" + std::to_string(i))},
    });
  }

  if (release) {
    const Vector2d end = start + delta;
    selectTool_->onMouseUp(app_, end);
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::MouseUp;
    event.mouseButton = 0;
    std::vector<repro::ReproEvent> events;
    events.push_back(std::move(event));
    appendRnrFrame(end, /*mouseButtonMask=*/0, std::move(events));
    app_.flushFrame();
    const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["release"] = json{
        {"point_doc", VectorToJson(end)},
        {"selection", selectedElementJson()},
        {"display_before_render", DisplayFrameJson(displayBeforeRender)},
        {"stages", RenderResultsJson(renderResults, &out, capture, "drag_selector/release")},
    };
  }

  out.body["final_selection"] = selectedElementJson();
  out.body["attached_image_count"] = out.images.size();
  return out;
}

ToolCallResult EditorControlSession::renderFrameTool(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  ToolCallResult out;
  CaptureOptions capture;
  if (!ReadCaptureOptions(arguments, true, &capture, &error)) {
    return MakeErrorResult(error);
  }
  std::vector<CapturedRenderResult> renderResults;
  if (!renderCurrentFrame(&renderResults, &error)) {
    return MakeErrorResult(error);
  }
  out.body = json{
      {"ok", true},
      {"source", sourceStateJson()},
      {"selection", selectedElementJson()},
      {"stages", RenderResultsJson(renderResults, &out, capture, "render_frame")},
  };
  out.body["attached_image_count"] = out.images.size();
  return out;
}

ToolCallResult EditorControlSession::sessionState(const json&) const {
  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"has_document", app_.hasDocument()},
      {"canvas", {{"width", canvasWidth_}, {"height", canvasHeight_}}},
      {"device_pixel_ratio", devicePixelRatio_},
      {"source", sourceStateJson()},
      {"selection", selectedElementJson()},
      {"worker_compositor_entity", EntityToJsonValue(asyncRenderer_.workerCompositorEntity())},
  };

  const auto state = asyncRenderer_.compositorState();
  out.body["compositor_state"] = json{
      {"active_hints_count", state.activeHintsCount},
      {"layer_count", state.layerCount},
      {"split_path_active", state.splitPathActive},
      {"split_static_layers_entity", EntityToJsonValue(state.splitStaticLayersEntity)},
      {"canvas_size", VectorToJson(state.canvasSize)},
      {"last_promote_refusal_reason", PromoteRefusalReasonName(state.lastPromoteRefusalReason)},
      {"last_promote_refusal_entity", EntityToJsonValue(state.lastPromoteRefusalEntity)},
  };

  json compositeTiles = json::array();
  int index = 0;
  for (const auto& tile : asyncRenderer_.compositorCompositeTiles()) {
    compositeTiles.push_back(CompositeTileSnapshotJson(tile, index++));
  }
  out.body["composite_tiles"] = std::move(compositeTiles);
  return out;
}

ToolCallResult EditorControlSession::startRnrRecording(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  if (rnrRecording_.active) {
    return MakeErrorResult("an .rnr recording is already active");
  }

  std::string outputPath;
  std::string svgPath;
  int windowWidth = canvasWidth_;
  int windowHeight = canvasHeight_;
  double displayScale = devicePixelRatio_;
  double frameDeltaMs = 1000.0 / 60.0;
  if (!ReadOptionalString(arguments, "output_path", "", &outputPath, &error) ||
      !ReadOptionalString(arguments, "svg_path", currentSourcePath_, &svgPath, &error) ||
      !ReadOptionalInt(arguments, "window_width", canvasWidth_, &windowWidth, &error) ||
      !ReadOptionalInt(arguments, "window_height", canvasHeight_, &windowHeight, &error) ||
      !ReadOptionalDouble(arguments, "display_scale", devicePixelRatio_, &displayScale, &error) ||
      !ReadOptionalDouble(arguments, "frame_delta_ms", 1000.0 / 60.0, &frameDeltaMs, &error)) {
    return MakeErrorResult(error);
  }
  if (svgPath.empty() || svgPath == "<memory>") {
    svgPath = "embedded.svg";
  }

  rnrRecording_ = RnrRecordingState{};
  rnrRecording_.active = true;
  if (!outputPath.empty()) {
    rnrRecording_.outputPath = std::filesystem::path(outputPath);
  }
  rnrRecording_.frameDeltaMs = frameDeltaMs > 0.0 ? frameDeltaMs : 1000.0 / 60.0;
  rnrRecording_.file.metadata.svgPath = svgPath;
  rnrRecording_.file.metadata.svgBasename = std::filesystem::path(svgPath).filename().string();
  rnrRecording_.file.metadata.svgContentHash = ReproContentHash(currentSourceText_);
  rnrRecording_.file.metadata.svgSource = currentSourceText_;
  rnrRecording_.file.metadata.windowWidth = windowWidth > 0 ? windowWidth : canvasWidth_;
  rnrRecording_.file.metadata.windowHeight = windowHeight > 0 ? windowHeight : canvasHeight_;
  rnrRecording_.file.metadata.displayScale = displayScale > 0.0 ? displayScale : 1.0;

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"active", true},
      {"output_path", outputPath.empty() ? nullptr : json(outputPath)},
      {"svg_path", rnrRecording_.file.metadata.svgPath},
      {"svg_basename", rnrRecording_.file.metadata.svgBasename},
      {"svg_hash", rnrRecording_.file.metadata.svgContentHash},
      {"embedded_svg_source", rnrRecording_.file.metadata.svgSource.has_value()},
      {"frame_count", rnrRecording_.file.frames.size()},
  };
  return out;
}

ToolCallResult EditorControlSession::stopRnrRecording(const json& arguments) {
  if (!rnrRecording_.active) {
    return MakeErrorResult("no .rnr recording is active");
  }

  std::string error;
  std::string outputPath;
  bool writeFile = true;
  if (!ReadOptionalString(arguments, "output_path", "", &outputPath, &error) ||
      !ReadOptionalBool(arguments, "write_file", true, &writeFile, &error)) {
    return MakeErrorResult(error);
  }

  std::optional<std::filesystem::path> path = rnrRecording_.outputPath;
  if (!outputPath.empty()) {
    path = std::filesystem::path(outputPath);
  }

  const std::size_t frameCount = rnrRecording_.file.frames.size();
  bool wroteFile = false;
  if (writeFile) {
    if (!path.has_value()) {
      return MakeErrorResult(
          "stop_rnr_recording requires output_path when the recording was "
          "started without one");
    }
    if (!repro::WriteReproFile(*path, rnrRecording_.file)) {
      return MakeErrorResult("failed to write .rnr file: " + path->string());
    }
    wroteFile = true;
  }

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"active", false},
      {"frame_count", frameCount},
      {"wrote_file", wroteFile},
      {"output_path", path.has_value() ? json(path->string()) : nullptr},
  };
  rnrRecording_ = RnrRecordingState{};
  return out;
}

ToolCallResult EditorControlSession::rnrRecordingState(const json&) const {
  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"active", rnrRecording_.active},
      {"frame_count", rnrRecording_.file.frames.size()},
      {"output_path",
       rnrRecording_.outputPath.has_value() ? json(rnrRecording_.outputPath->string()) : nullptr},
      {"svg_path", rnrRecording_.file.metadata.svgPath},
      {"svg_basename", rnrRecording_.file.metadata.svgBasename},
      {"svg_hash", rnrRecording_.file.metadata.svgContentHash},
      {"embedded_svg_source", rnrRecording_.file.metadata.svgSource.has_value()},
  };
  return out;
}

ToolCallResult EditorControlSession::replayRnr(const json& arguments) {
  std::string error;
  std::string rnrPathString;
  std::string svgPathOverride;
  bool renderEachFrame = true;
  bool simulateEditorShellFrameLoop = false;
  bool glReadback = false;
  bool includeGlImages = true;
  bool glVisible = false;
  bool glPace = true;
  int glCaptureFrame = -1;
  int glCaptureLeftMouseDown = 0;
  int glMaxFrame = -1;
  std::string glCrop = "full";
  std::string glOutputDir;
  bool includeFrameResults = true;
  bool includeDisplayDiff = false;
  int maxFrameResults = 200;
  int stopAfterMouseUps = 0;
  int comparePresentedAfterLeftMouseDown = 0;
  int comparePresentedFrameOffsetAfterLeftMouseDown = 0;
  CaptureOptions capture;
  if (!ReadRequiredString(arguments, "rnr_path", &rnrPathString, &error) ||
      !ReadOptionalString(arguments, "svg_path", "", &svgPathOverride, &error) ||
      !ReadOptionalBool(arguments, "render_each_frame", true, &renderEachFrame, &error) ||
      !ReadOptionalBool(arguments, "simulate_editor_shell_frame_loop", false,
                        &simulateEditorShellFrameLoop, &error) ||
      !ReadOptionalBool(arguments, "gl_readback", false, &glReadback, &error) ||
      !ReadOptionalBool(arguments, "include_gl_images", true, &includeGlImages, &error) ||
      !ReadOptionalBool(arguments, "gl_visible", false, &glVisible, &error) ||
      !ReadOptionalBool(arguments, "gl_pace", true, &glPace, &error) ||
      !ReadOptionalInt(arguments, "gl_capture_frame", -1, &glCaptureFrame, &error) ||
      !ReadOptionalInt(arguments, "gl_capture_left_mousedown", 0, &glCaptureLeftMouseDown,
                       &error) ||
      !ReadOptionalInt(arguments, "gl_max_frame", -1, &glMaxFrame, &error) ||
      !ReadOptionalString(arguments, "gl_crop", "full", &glCrop, &error) ||
      !ReadOptionalString(arguments, "gl_output_dir", "", &glOutputDir, &error) ||
      !ReadOptionalBool(arguments, "include_frame_results", true, &includeFrameResults, &error) ||
      !ReadOptionalBool(arguments, "include_display_diff", false, &includeDisplayDiff, &error) ||
      !ReadOptionalInt(arguments, "max_frame_results", 200, &maxFrameResults, &error) ||
      !ReadOptionalInt(arguments, "stop_after_mouse_ups", 0, &stopAfterMouseUps, &error) ||
      !ReadOptionalInt(arguments, "compare_presented_after_left_mouse_down", 0,
                       &comparePresentedAfterLeftMouseDown, &error) ||
      !ReadOptionalInt(arguments, "compare_presented_frame_offset_after_left_mouse_down", 0,
                       &comparePresentedFrameOffsetAfterLeftMouseDown, &error) ||
      !ReadCaptureOptions(arguments, false, &capture, &error)) {
    return MakeErrorResult(error);
  }

  if (glReadback) {
    const std::optional<repro::GlRnrReplayCropMode> cropMode =
        repro::ParseGlRnrReplayCropMode(glCrop);
    if (!cropMode.has_value()) {
      return MakeErrorResult("gl_crop must be one of: full, render-pane, document-canvas");
    }
    if (glCaptureFrame < -1) {
      return MakeErrorResult("gl_capture_frame must be non-negative");
    }
    if (glCaptureLeftMouseDown < 0) {
      return MakeErrorResult("gl_capture_left_mousedown must be positive");
    }
    if (glMaxFrame < -1) {
      return MakeErrorResult("gl_max_frame must be non-negative");
    }

    repro::GlRnrReplayOptions replayOptions;
    replayOptions.rnrPath = std::filesystem::path(rnrPathString);
    if (!svgPathOverride.empty()) {
      replayOptions.svgPathOverride = std::filesystem::path(svgPathOverride);
    }
    replayOptions.outputDir = glOutputDir.empty() ? (DiagnosticOutputDir() / "gl_rnr_replay")
                                                  : std::filesystem::path(glOutputDir);
    if (glCaptureFrame >= 0) {
      replayOptions.captureFrames.insert(static_cast<std::uint64_t>(glCaptureFrame));
      if (glMaxFrame < 0) {
        replayOptions.maxFrame = static_cast<std::uint64_t>(glCaptureFrame);
      }
    }
    if (glCaptureLeftMouseDown > 0) {
      replayOptions.captureLeftMouseDownOrdinal = glCaptureLeftMouseDown;
    }
    if (glMaxFrame >= 0) {
      replayOptions.maxFrame = static_cast<std::uint64_t>(glMaxFrame);
    }
    replayOptions.cropMode = *cropMode;
    replayOptions.pace = glPace;
    replayOptions.visible = glVisible;

    repro::GlRnrReplayResult replayResult;
    if (!repro::RunGlRnrReplay(replayOptions, &replayResult, &error)) {
      return MakeErrorResult(error);
    }

    ToolCallResult out;
    out.body = json{
        {"ok", true},
        {"mode", "gl_readback"},
        {"rnr_path", rnrPathString},
        {"svg_path", svgPathOverride.empty() ? json(nullptr) : json(svgPathOverride)},
        {"output_dir", replayOptions.outputDir.string()},
        {"crop", glCrop},
        {"capture_count", replayResult.captures.size()},
        {"captures", json::array()},
    };
    for (const repro::GlRnrReplayCapture& captureResult : replayResult.captures) {
      json captureJson{
          {"frame_index", captureResult.frameIndex},
          {"reason", captureResult.reason},
          {"path", captureResult.path.string()},
      };
      if (includeGlImages) {
        AttachPngFile(&out,
                      "replay_rnr_gl_frame_" + std::to_string(captureResult.frameIndex) + "_" +
                          captureResult.reason,
                      captureResult.path, capture.embedPngBase64, &captureJson);
      }
      out.body["captures"].push_back(std::move(captureJson));
    }
    out.body["attached_image_count"] = out.images.size();
    return out;
  }

  const std::filesystem::path rnrPath(rnrPathString);
  std::optional<repro::ReproFile> replay = repro::ReadReproFile(rnrPath);
  if (!replay.has_value()) {
    return MakeErrorResult("failed to read .rnr file: " + rnrPathString);
  }

  std::filesystem::path svgDisplayPath;
  std::string liveSource;
  bool usedEmbeddedSvgSource = false;
  if (!svgPathOverride.empty()) {
    svgDisplayPath = std::filesystem::path(svgPathOverride);
    std::optional<std::string> source = ReadTextFile(svgDisplayPath);
    if (!source.has_value()) {
      return MakeErrorResult("failed to open SVG for .rnr replay: " + svgDisplayPath.string());
    }
    liveSource = std::move(*source);
  } else if (replay->metadata.svgSource.has_value()) {
    svgDisplayPath = !replay->metadata.svgBasename.empty()
                         ? std::filesystem::path(replay->metadata.svgBasename)
                         : std::filesystem::path(replay->metadata.svgPath).filename();
    if (svgDisplayPath.empty()) {
      svgDisplayPath = "embedded.svg";
    }
    liveSource = *replay->metadata.svgSource;
    usedEmbeddedSvgSource = true;
  } else {
    std::optional<std::filesystem::path> resolvedSvgPath =
        resolveRnrSvgPath(rnrPath, replay->metadata.svgPath);
    if (!resolvedSvgPath.has_value()) {
      return MakeErrorResult("failed to resolve SVG path from .rnr metadata: " +
                             replay->metadata.svgPath);
    }
    svgDisplayPath = *resolvedSvgPath;
    std::optional<std::string> source = ReadTextFile(svgDisplayPath);
    if (!source.has_value()) {
      return MakeErrorResult("failed to open SVG for .rnr replay: " + svgDisplayPath.string());
    }
    liveSource = std::move(*source);
  }

  LoadOptions loadOptions;
  loadOptions.canvasWidth =
      replay->metadata.windowWidth > 0 ? replay->metadata.windowWidth : kDefaultCanvasWidth;
  loadOptions.canvasHeight =
      replay->metadata.windowHeight > 0 ? replay->metadata.windowHeight : kDefaultCanvasHeight;
  loadOptions.devicePixelRatio =
      replay->metadata.displayScale > 0.0 ? replay->metadata.displayScale : 1.0;
  loadOptions.renderAfterLoad = false;
  ToolCallResult load = loadSvgSource(liveSource, loadOptions, svgDisplayPath.string());
  if (load.isError || !load.body.value("ok", false)) {
    return load;
  }

  ViewportState viewport;
  viewport.devicePixelRatio = loadOptions.devicePixelRatio;
  viewport.paneOrigin = Vector2d::Zero();
  viewport.paneSize = Vector2d(static_cast<double>(loadOptions.canvasWidth),
                               static_cast<double>(loadOptions.canvasHeight));
  viewport.documentViewBox = DocumentViewBoxOr(
      app_.document().document(),
      Box2d::FromXYWH(0.0, 0.0, loadOptions.canvasWidth, loadOptions.canvasHeight));
  viewport.resetTo100Percent();
  for (const repro::ReproFrame& frame : replay->frames) {
    if (frame.viewport.has_value()) {
      ApplyReproViewport(&viewport, *frame.viewport);
      break;
    }
  }
  (void)syncCanvasSize(viewport);
  app_.flushFrame();

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"rnr_path", rnrPathString},
      {"svg_path", svgDisplayPath.string()},
      {"embedded_svg_source", usedEmbeddedSvgSource},
      {"svg_hash", replay->metadata.svgContentHash},
      {"frame_count", replay->frames.size()},
      {"rendered_frame_count", 0},
      {"mouse_up_count", 0},
      {"stopped_early", false},
      {"frames", json::array()},
  };

  if (simulateEditorShellFrameLoop) {
    out.body["mode"] = "editor_shell_frame_loop";

    struct PendingClick {
      Vector2d documentPoint = Vector2d::Zero();
      MouseModifiers modifiers;
      int leftMouseDownOrdinal = 0;
      bool dispatched = false;
    };

    std::optional<PendingClick> pendingClick;
    bool leftButtonHeld = false;
    int processedFrameCount = 0;
    int skippedIdleFrameCount = 0;
    int renderedFrameCount = 0;
    int storedFrameResults = 0;
    int mouseUpCount = 0;
    int leftMouseDownCount = 0;
    int targetLeftMouseDownFrameOffset = -1;
    bool comparedTargetPresentedFrame = false;
    PresentationRenderScheduler renderScheduler;

    const auto pollRenderResult = [&]() {
      std::optional<RenderResult> result = asyncRenderer_.pollResult();
      if (!result.has_value()) {
        return false;
      }

      recordDisplayFrame(*result);
      renderScheduler.noteRenderCompleted(result->version, asyncRenderer_.lastDocumentCanvasSize(),
                                          result->rasterViewport);
      ++renderedFrameCount;
      return true;
    };

    const auto waitUntilIdleAndRecord = [&]() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (std::chrono::steady_clock::now() < deadline) {
        const bool polled = pollRenderResult();
        if (!asyncRenderer_.isBusy()) {
          while (pollRenderResult()) {}
          return true;
        }
        if (!polled) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      return false;
    };

    const auto maybeRequestGuiRender = [&]() {
      if (asyncRenderer_.isBusy() || !app_.hasDocument() || viewport.paneSize.x <= 0.0 ||
          viewport.paneSize.y <= 0.0) {
        return false;
      }

      (void)syncCanvasSize(viewport);
      (void)app_.flushFrame();

      const EditorRasterViewport currentRasterViewport = viewport.rasterViewport();
      const Vector2i currentCanvasSize = currentRasterViewport.outputSizePx;
      const std::uint64_t currentVersion = app_.document().currentFrameVersion();
      const std::optional<SelectTool::ActiveDragPreview> dragPreview =
          selectTool_->activeDragPreview();
      const Entity selectedEntity = SelectedGraphicsEntity(app_);
      const PresentationRenderScheduleDecision schedule = renderScheduler.evaluate(
          displayPresentation_, PresentationRenderScheduleInput{
                                    .selectedEntity = selectedEntity,
                                    .activeDragPreview = dragPreview,
                                    .currentVersion = currentVersion,
                                    .currentCanvasSize = currentCanvasSize,
                                    .currentRasterViewport = currentRasterViewport,
                                });
      if (!schedule.shouldRequestRender()) {
        return false;
      }

      RenderRequest request(renderer_, app_.document().document());
      request.version = currentVersion;
      request.documentGeneration = app_.document().documentGeneration();
      request.structuralRemap = app_.document().consumePendingStructuralRemap();
      request.rasterViewport = currentRasterViewport;
      if (app_.selectedElement().has_value()) {
        request.selection = *app_.selectedElement();
      }
      request.selectedEntity = selectedEntity;
      if (schedule.dragPreview.has_value()) {
        request.dragPreview = *schedule.dragPreview;
      }
      asyncRenderer_.requestRender(request);
      return true;
    };

    const auto replayStart = std::chrono::steady_clock::now();
    for (const repro::ReproFrame& frame : replay->frames) {
      const auto targetTime =
          replayStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(frame.timestampSeconds));
      while (std::chrono::steady_clock::now() < targetTime) {
        (void)pollRenderResult();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      ++processedFrameCount;
      const bool polledAtFrameStart = pollRenderResult();
      if (!asyncRenderer_.isBusy()) {
        (void)DrainWritebackAndReparseSource(&app_, selectTool_.get(), &liveSource);
        (void)app_.flushFrame();
      }

      if (frame.viewport.has_value()) {
        ApplyReproViewport(&viewport, *frame.viewport);
      }
      const bool preInputRenderRequested = maybeRequestGuiRender();

      const Vector2d mouseScreen(frame.mouseX, frame.mouseY);
      const Vector2d mouseDoc = frame.mouseDocX.has_value() && frame.mouseDocY.has_value()
                                    ? Vector2d(*frame.mouseDocX, *frame.mouseDocY)
                                    : viewport.screenToDocument(mouseScreen);
      const bool nowHeld = (frame.mouseButtonMask & 1) != 0;
      bool frameHadLeftMouseDown = false;
      bool cancelledForPendingClick = false;
      for (const repro::ReproEvent& event : frame.events) {
        if (event.kind == repro::ReproEvent::Kind::MouseDown && event.mouseButton == 0) {
          ++leftMouseDownCount;
          frameHadLeftMouseDown = true;
          pendingClick = PendingClick{
              .documentPoint = mouseDoc,
              .modifiers = MouseModifiers{.shift = (frame.modifiers & 2) != 0},
              .leftMouseDownOrdinal = leftMouseDownCount,
          };
        }
      }
      if (comparePresentedAfterLeftMouseDown > 0 && !comparedTargetPresentedFrame) {
        if (frameHadLeftMouseDown && leftMouseDownCount == comparePresentedAfterLeftMouseDown) {
          targetLeftMouseDownFrameOffset = 0;
        } else if (targetLeftMouseDownFrameOffset >= 0) {
          ++targetLeftMouseDownFrameOffset;
        }
      }

      if (pendingClick.has_value() && !pendingClick->dispatched && asyncRenderer_.isBusy()) {
        asyncRenderer_.cancelInFlight();
        cancelledForPendingClick = true;
      }

      if (!asyncRenderer_.isBusy()) {
        if (pendingClick.has_value() && !pendingClick->dispatched) {
          selectTool_->onMouseDown(app_, pendingClick->documentPoint, pendingClick->modifiers);
          pendingClick->dispatched = true;
        }

        for (const repro::ReproEvent& event : frame.events) {
          if (event.kind == repro::ReproEvent::Kind::MouseUp && event.mouseButton == 0) {
            selectTool_->onMouseUp(app_, mouseDoc);
            ++mouseUpCount;
          }
        }

        if (selectTool_->isDragging() && nowHeld && leftButtonHeld) {
          selectTool_->onMouseMove(app_, mouseDoc, /*buttonHeld=*/true);
        }
        (void)app_.flushFrame();
      } else {
        for (const repro::ReproEvent& event : frame.events) {
          if (event.kind == repro::ReproEvent::Kind::MouseUp && event.mouseButton == 0) {
            ++mouseUpCount;
          }
        }
      }

      leftButtonHeld = nowHeld;
      const bool postInputRenderRequested = maybeRequestGuiRender();
      const DisplayFrameSnapshot presentedFrame = currentDisplayFrame();
      const Box2d viewBox = DocumentViewBoxOr(
          app_.document().document(), Box2d::FromXYWH(0.0, 0.0, canvasWidth_, canvasHeight_));
      const std::optional<svg::RendererBitmap> presentedBitmap =
          displayTextures_.composeDisplayFrame(presentedFrame, viewBox,
                                               app_.document().document().canvasSize());

      json frameJson{
          {"frame_index", frame.index},
          {"mouse_doc", VectorToJson(mouseDoc)},
          {"events", ReproEventsJson(frame.events)},
          {"left_mouse_down_ordinal", frameHadLeftMouseDown ? leftMouseDownCount : 0},
          {"left_mouse_down_compare_offset", targetLeftMouseDownFrameOffset >= 0
                                                 ? json(targetLeftMouseDownFrameOffset)
                                                 : json(nullptr)},
          {"worker_busy_after_frame", asyncRenderer_.isBusy()},
          {"pending_click",
           pendingClick.has_value()
               ? json{{"left_mouse_down_ordinal", pendingClick->leftMouseDownOrdinal},
                      {"dispatched", pendingClick->dispatched}}
               : json(nullptr)},
          {"polled_render_result_at_frame_start", polledAtFrameStart},
          {"pre_input_render_requested", preInputRenderRequested},
          {"post_input_render_requested", postInputRenderRequested},
          {"cancelled_render_for_pending_click", cancelledForPendingClick},
          {"selection", selectedElementJson()},
          {"presented_frame", DisplayFrameJson(presentedFrame)},
          {"presented_frame_bitmap",
           presentedBitmap.has_value() ? BitmapSummary(*presentedBitmap) : json(nullptr)},
      };

      const bool shouldComparePresentedFrame =
          comparePresentedAfterLeftMouseDown > 0 && !comparedTargetPresentedFrame &&
          targetLeftMouseDownFrameOffset == comparePresentedFrameOffsetAfterLeftMouseDown;
      if (shouldComparePresentedFrame && includeDisplayDiff) {
        if (!waitUntilIdleAndRecord()) {
          return MakeErrorResult(
              "timed out waiting for async renderer before presented-frame "
              "comparison");
        }
        if (pendingClick.has_value() && !pendingClick->dispatched) {
          selectTool_->onMouseDown(app_, pendingClick->documentPoint, pendingClick->modifiers);
          pendingClick->dispatched = true;
          (void)app_.flushFrame();
        }

        std::vector<CapturedRenderResult> renderResults;
        if (!renderCurrentFrame(&renderResults, &error)) {
          return MakeErrorResult("final render failed for presented-frame comparison: " + error);
        }
        ++renderedFrameCount;
        const DisplayFrameSnapshot* finalDisplayFrame = nullptr;
        for (const CapturedRenderResult& result : renderResults) {
          finalDisplayFrame = &result.displayFrame;
        }
        if (finalDisplayFrame != nullptr) {
          const std::optional<svg::RendererBitmap> finalPresentedBitmap =
              displayTextures_.composeDisplayFrame(*finalDisplayFrame, viewBox,
                                                   app_.document().document().canvasSize());
          frameJson["eventual_final_presented_frame"] = DisplayFrameJson(*finalDisplayFrame);
          frameJson["eventual_final_presented_frame_bitmap"] =
              finalPresentedBitmap.has_value() ? BitmapSummary(*finalPresentedBitmap)
                                               : json(nullptr);
          if (finalPresentedBitmap.has_value()) {
            json diff = BitmapDiffSummary(presentedBitmap, *finalPresentedBitmap);
            AddBitmapDiffArtifacts(&diff, presentedBitmap, *finalPresentedBitmap,
                                   "replay_rnr_gui_frame_" + std::to_string(frame.index));
            frameJson["presented_frame_diff_from_eventual_final"] = std::move(diff);
          } else {
            frameJson["presented_frame_diff_from_eventual_final"] = json{
                {"available", false},
                {"actual_bitmap",
                 presentedBitmap.has_value() ? BitmapSummary(*presentedBitmap) : json(nullptr)},
                {"expected_bitmap", nullptr},
            };
          }
        }
        out.body["stopped_early"] = true;
        comparedTargetPresentedFrame = true;
      }

      if (includeFrameResults && storedFrameResults < maxFrameResults) {
        out.body["frames"].push_back(std::move(frameJson));
        ++storedFrameResults;
      }

      if (shouldComparePresentedFrame) {
        break;
      }
      if (stopAfterMouseUps > 0 && mouseUpCount >= stopAfterMouseUps) {
        out.body["stopped_early"] = true;
        break;
      }
    }

    out.body["processed_frame_count"] = processedFrameCount;
    out.body["skipped_idle_frame_count"] = skippedIdleFrameCount;
    out.body["rendered_frame_count"] = renderedFrameCount;
    out.body["stored_frame_result_count"] = storedFrameResults;
    out.body["mouse_up_count"] = mouseUpCount;
    out.body["left_mouse_down_count"] = leftMouseDownCount;
    out.body["final_selection"] = selectedElementJson();
    out.body["attached_image_count"] = out.images.size();
    return out;
  }

  bool leftButtonHeld = false;
  int processedFrameCount = 0;
  int skippedIdleFrameCount = 0;
  int renderedFrameCount = 0;
  int storedFrameResults = 0;
  int mouseUpCount = 0;
  for (const repro::ReproFrame& frame : replay->frames) {
    bool frameNeedsRender = drainPendingWritebacks();
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;
    const bool viewportUnchanged =
        !frame.viewport.has_value() || ReproViewportMatches(viewport, *frame.viewport);
    if (!frameNeedsRender && !nowHeld && !leftButtonHeld && frame.events.empty() &&
        viewportUnchanged) {
      ++skippedIdleFrameCount;
      continue;
    }
    ++processedFrameCount;

    if (frame.viewport.has_value()) {
      ApplyReproViewport(&viewport, *frame.viewport);
      frameNeedsRender |= syncCanvasSize(viewport);
    }

    const Vector2d mouseScreen(frame.mouseX, frame.mouseY);
    const Vector2d mouseDoc = frame.mouseDocX.has_value() && frame.mouseDocY.has_value()
                                  ? Vector2d(*frame.mouseDocX, *frame.mouseDocY)
                                  : viewport.screenToDocument(mouseScreen);

    for (const repro::ReproEvent& event : frame.events) {
      if (event.kind == repro::ReproEvent::Kind::MouseDown && event.mouseButton == 0) {
        selectTool_->onMouseDown(app_, mouseDoc,
                                 MouseModifiers{.shift = (frame.modifiers & 2) != 0});
        frameNeedsRender = true;
      } else if (event.kind == repro::ReproEvent::Kind::MouseUp && event.mouseButton == 0) {
        selectTool_->onMouseUp(app_, mouseDoc);
        ++mouseUpCount;
        frameNeedsRender = true;
      }
    }

    if (nowHeld && leftButtonHeld) {
      selectTool_->onMouseMove(app_, mouseDoc, /*buttonHeld=*/true);
      frameNeedsRender = true;
    }
    leftButtonHeld = nowHeld;
    frameNeedsRender |= app_.flushFrame();

    if (renderEachFrame && frameNeedsRender) {
      const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
      const Box2d viewBox = DocumentViewBoxOr(
          app_.document().document(), Box2d::FromXYWH(0.0, 0.0, canvasWidth_, canvasHeight_));
      const std::optional<svg::RendererBitmap> displayBeforeRenderBitmap =
          includeDisplayDiff
              ? displayTextures_.composeDisplayFrame(displayBeforeRender, viewBox,
                                                     app_.document().document().canvasSize())
              : std::nullopt;
      std::vector<CapturedRenderResult> renderResults;
      if (!renderCurrentFrame(&renderResults, &error)) {
        return MakeErrorResult("render failed while replaying .rnr frame " +
                               std::to_string(frame.index) + ": " + error);
      }
      ++renderedFrameCount;
      if (includeFrameResults && storedFrameResults < maxFrameResults) {
        const DisplayFrameSnapshot* finalDisplayFrame = nullptr;
        for (const CapturedRenderResult& result : renderResults) {
          finalDisplayFrame = &result.displayFrame;
        }
        std::optional<svg::RendererBitmap> finalDisplayBitmap;
        if (finalDisplayFrame != nullptr) {
          finalDisplayBitmap = displayTextures_.composeDisplayFrame(
              *finalDisplayFrame, viewBox, app_.document().document().canvasSize());
        }

        json frameJson{
            {"frame_index", frame.index},
            {"mouse_doc", VectorToJson(mouseDoc)},
            {"events", ReproEventsJson(frame.events)},
            {"selection", selectedElementJson()},
            {"display_before_render", DisplayFrameJson(displayBeforeRender)},
            {"eventual_final_presented_frame",
             finalDisplayFrame != nullptr ? DisplayFrameJson(*finalDisplayFrame) : json(nullptr)},
            {"eventual_final_presented_frame_bitmap",
             finalDisplayBitmap.has_value() ? BitmapSummary(*finalDisplayBitmap) : json(nullptr)},
            {"stages", RenderResultsJson(renderResults, &out, capture,
                                         "replay_rnr/frame_" + std::to_string(frame.index))},
        };
        if (includeDisplayDiff && finalDisplayBitmap.has_value()) {
          json diff = BitmapDiffSummary(displayBeforeRenderBitmap, *finalDisplayBitmap);
          AddBitmapDiffArtifacts(
              &diff, displayBeforeRenderBitmap, *finalDisplayBitmap,
              "replay_rnr_frame_" + std::to_string(frame.index) + "_display_before_render");
          frameJson["display_before_render_diff_from_final"] = std::move(diff);
        }
        out.body["frames"].push_back(std::move(frameJson));
        ++storedFrameResults;
      }
    }

    if (stopAfterMouseUps > 0 && mouseUpCount >= stopAfterMouseUps) {
      out.body["stopped_early"] = true;
      break;
    }
  }

  if (!renderEachFrame) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult("final render failed after .rnr replay: " + error);
    }
    ++renderedFrameCount;
    if (includeFrameResults && storedFrameResults < maxFrameResults) {
      out.body["frames"].push_back(json{
          {"frame_index", nullptr},
          {"mouse_doc", nullptr},
          {"selection", selectedElementJson()},
          {"stages", RenderResultsJson(renderResults, &out, capture, "replay_rnr/final")},
      });
    }
  }

  out.body["processed_frame_count"] = processedFrameCount;
  out.body["skipped_idle_frame_count"] = skippedIdleFrameCount;
  out.body["rendered_frame_count"] = renderedFrameCount;
  out.body["stored_frame_result_count"] = storedFrameResults;
  out.body["mouse_up_count"] = mouseUpCount;
  out.body["final_selection"] = selectedElementJson();
  out.body["attached_image_count"] = out.images.size();
  return out;
}

bool EditorControlSession::drainPendingWritebacks() {
  std::vector<EditorApp::CompletedTransformWriteback> transformWritebacks;
  if (auto completed = selectTool_->consumeCompletedDragWriteback(); completed.has_value()) {
    transformWritebacks.push_back(EditorApp::CompletedTransformWriteback{
        .target = std::move(completed->target),
        .transform = completed->transform,
    });
    for (auto& extra : completed->extras) {
      transformWritebacks.push_back(EditorApp::CompletedTransformWriteback{
          .target = std::move(extra.target),
          .transform = extra.transform,
      });
    }
  }
  if (auto completed = app_.consumeTransformWriteback(); completed.has_value()) {
    transformWritebacks.push_back(std::move(*completed));
  }

  bool changed = false;
  if (app_.hasDocument() && app_.document().document().hasSourceStore()) {
    svg::SVGDocument& document = app_.document().document();
    for (const EditorApp::CompletedTransformWriteback& writeback : transformWritebacks) {
      std::optional<svg::SVGElement> element =
          resolveAttributeWritebackTarget(document, writeback.target);
      if (!element.has_value()) {
        continue;
      }

      if (writeback.restoreSourceTransformAttributeValue) {
        if (writeback.sourceTransformAttributeValue.has_value()) {
          (void)document.setElementAttribute(*element, "transform",
                                             *writeback.sourceTransformAttributeValue);
        } else {
          (void)document.removeElementAttribute(*element, "transform");
        }
      } else {
        const RcString serialized = toSVGTransformString(writeback.transform);
        if (std::string_view(serialized).empty()) {
          (void)document.removeElementAttribute(*element, "transform");
        } else {
          (void)document.setElementAttribute(*element, "transform", serialized);
        }
      }
      changed = true;
    }
  }

  const std::vector<EditorApp::CompletedElementRemoveWriteback> elementRemoveWritebacks =
      app_.consumeElementRemoveWritebacks();
  changed = changed || !elementRemoveWritebacks.empty();
  if (changed && app_.hasDocument()) {
    currentSourceText_ = std::string(app_.document().document().source());
    ++sourceRevision_;
    loadedSourceRevision_ = sourceRevision_;
    lastParseError_.reset();
  }
  return changed;
}

bool EditorControlSession::renderCurrentFrame(std::vector<CapturedRenderResult>* results,
                                              std::string* error) {
  results->clear();
  if (!ensureDocumentLoaded(error)) {
    return false;
  }

  const std::optional<SelectTool::ActiveDragPreview> activePreview =
      selectTool_->activeDragPreview();
  const Entity selectedEntity = SelectedGraphicsEntity(app_);
  displayPresentation_.clearSettlingIfSelectionChanged(selectedEntity, activePreview.has_value());

  RenderRequest request(renderer_, app_.document().document());
  request.version = nextRenderVersion_++;
  request.documentGeneration = app_.document().documentGeneration();
  request.structuralRemap = app_.document().consumePendingStructuralRemap();

  if (app_.selectedElement().has_value()) {
    request.selection = *app_.selectedElement();
    request.selectedEntity = selectedEntity;
  }

  if (activePreview.has_value()) {
    request.dragPreview = RenderRequest::DragPreview{
        .entity = activePreview->entity,
        .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
    };
  } else if (request.selectedEntity != entt::null) {
    request.dragPreview = RenderRequest::DragPreview{
        .entity = request.selectedEntity,
        .interactionKind = svg::compositor::InteractionHint::Selection,
    };
  }

  asyncRenderer_.requestRender(request);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto result = asyncRenderer_.pollResult(); result.has_value()) {
      DisplayFrameSnapshot displayFrame = recordDisplayFrame(*result);
      results->push_back(CapturedRenderResult{
          .renderResult = std::move(*result),
          .displayFrame = std::move(displayFrame),
      });
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  *error = "timed out waiting for async render result";
  return false;
}

EditorControlSession::DisplayFrameSnapshot EditorControlSession::recordDisplayFrame(
    const RenderResult& result) {
  if (result.compositedPreview.has_value() && result.compositedPreview->valid()) {
    displayTextures_.uploadComposited(*result.compositedPreview);
    displayPresentation_.noteCachedTextures(result.compositedPreview->entity, result.version,
                                            app_.document().document().canvasSize());
  }

  return currentDisplayFrame();
}

EditorControlSession::DisplayFrameSnapshot EditorControlSession::currentDisplayFrame() const {
  const std::optional<SelectTool::ActiveDragPreview> activePreview =
      selectTool_->activeDragPreview();
  const std::optional<SelectTool::ActiveDragPreview> displayedPreview =
      displayPresentation_.presentationPreview(activePreview);
  const CompositedPresentation::DiagnosticsSnapshot presentation =
      displayPresentation_.diagnostics();

  DisplayFrameSnapshot frame;
  frame.hasCachedTiles = presentation.hasCachedTextures;
  frame.cachedEntity = presentation.cachedEntity;
  frame.hasActiveDragPreview = activePreview.has_value();
  frame.activeDragPreview = activePreview;
  frame.displayedDragPreview = displayedPreview;
  frame.displayedEntity = displayedPreview.has_value() ? displayedPreview->entity : entt::null;
  if (!displayTextures_.tiles().empty()) {
    frame.path = "tiles";
    frame.tiles = displayTextures_.tiles();
  } else {
    frame.path = "empty";
  }

  return frame;
}

void EditorControlSession::appendRnrFrame(const Vector2d& documentPoint, int mouseButtonMask,
                                          std::vector<repro::ReproEvent> events) {
  if (!rnrRecording_.active) {
    return;
  }

  const Vector2d screenPoint = currentRecordingScreenPoint(documentPoint);
  repro::ReproFrame frame;
  frame.index = rnrRecording_.nextFrameIndex++;
  frame.timestampSeconds = rnrRecording_.timestampSeconds;
  frame.deltaMs = rnrRecording_.frameDeltaMs;
  frame.mouseX = screenPoint.x;
  frame.mouseY = screenPoint.y;
  frame.mouseDocX = documentPoint.x;
  frame.mouseDocY = documentPoint.y;
  frame.mouseButtonMask = mouseButtonMask;
  frame.modifiers = 0;
  frame.viewport = currentReproViewport();
  frame.events = std::move(events);
  rnrRecording_.file.frames.push_back(std::move(frame));
  rnrRecording_.timestampSeconds += rnrRecording_.frameDeltaMs / 1000.0;
}

repro::ReproViewport EditorControlSession::currentReproViewport() const {
  const Box2d fallback = Box2d::FromXYWH(0.0, 0.0, canvasWidth_, canvasHeight_);
  const Box2d viewBox =
      app_.hasDocument() ? DocumentViewBoxOr(app_.document().document(), fallback) : fallback;
  const Vector2d viewBoxSize = viewBox.size();
  const double deviceScaleX =
      viewBoxSize.x > 0.0 ? static_cast<double>(canvasWidth_) / viewBoxSize.x : 1.0;
  const double zoom = devicePixelRatio_ > 0.0 ? deviceScaleX / devicePixelRatio_ : deviceScaleX;
  const Vector2d paneSize(viewBoxSize.x * zoom, viewBoxSize.y * zoom);
  const Vector2d panDoc = (viewBox.topLeft + viewBox.bottomRight) * 0.5;
  const Vector2d panScreen = paneSize * 0.5;

  repro::ReproViewport viewport;
  viewport.paneOriginX = 0.0;
  viewport.paneOriginY = 0.0;
  viewport.paneSizeW = paneSize.x;
  viewport.paneSizeH = paneSize.y;
  viewport.devicePixelRatio = devicePixelRatio_;
  viewport.zoom = zoom;
  viewport.panDocX = panDoc.x;
  viewport.panDocY = panDoc.y;
  viewport.panScreenX = panScreen.x;
  viewport.panScreenY = panScreen.y;
  viewport.viewBoxX = viewBox.topLeft.x;
  viewport.viewBoxY = viewBox.topLeft.y;
  viewport.viewBoxW = viewBox.width();
  viewport.viewBoxH = viewBox.height();
  return viewport;
}

Vector2d EditorControlSession::currentRecordingScreenPoint(const Vector2d& documentPoint) const {
  const repro::ReproViewport viewport = currentReproViewport();
  const Vector2d panDoc(viewport.panDocX, viewport.panDocY);
  const Vector2d panScreen(viewport.panScreenX, viewport.panScreenY);
  return panScreen + (documentPoint - panDoc) * viewport.zoom;
}

std::optional<std::filesystem::path> EditorControlSession::resolveRnrSvgPath(
    const std::filesystem::path& rnrPath, std::string_view recordingSvgPath) const {
  const std::filesystem::path direct(recordingSvgPath);
  std::error_code ec;
  if (std::filesystem::exists(direct, ec)) {
    return direct;
  }

  ec.clear();
  const std::filesystem::path alongside = rnrPath.parent_path() / direct;
  if (std::filesystem::exists(alongside, ec)) {
    return alongside;
  }

  ec.clear();
  const std::filesystem::path fromCurrent = std::filesystem::path(".") / direct;
  if (std::filesystem::exists(fromCurrent, ec)) {
    return fromCurrent;
  }

  return std::nullopt;
}

bool EditorControlSession::syncCanvasSize(const ViewportState& viewport) {
  if (!app_.hasDocument()) {
    return false;
  }

  const Vector2i desired = viewport.desiredCanvasSize();
  const Vector2i current = app_.document().document().canvasSize();
  if (desired == current) {
    return false;
  }

  app_.document().document().setCanvasSize(desired.x, desired.y);
  canvasWidth_ = desired.x;
  canvasHeight_ = desired.y;
  devicePixelRatio_ = viewport.devicePixelRatio;
  return true;
}

std::optional<svg::SVGElement> EditorControlSession::querySelector(std::string_view selector) {
  return app_.document().document().querySelector(selector);
}

std::optional<Box2d> EditorControlSession::elementWorldBounds(
    const svg::SVGElement& element) const {
  std::optional<Box2d> result;
  const std::optional<svg::SVGGeometryElement> geometry =
      element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
        if (!element.isa<svg::SVGGeometryElement>()) {
          return std::optional<svg::SVGGeometryElement>();
        }

        return std::optional(element.cast<svg::SVGGeometryElement>());
      });
  if (geometry.has_value()) {
    result = geometry->worldBounds();
  }

  std::optional<svg::SVGElement> child = element.withReadAccess(
      [&element](svg::DocumentReadAccess&, EntityHandle) { return element.firstChild(); });
  while (child.has_value()) {
    const svg::SVGElement currentChild = *child;
    if (auto childBounds = elementWorldBounds(*child); childBounds.has_value()) {
      if (result.has_value()) {
        result->addBox(*childBounds);
      } else {
        result = *childBounds;
      }
    }

    child = currentChild.withReadAccess([&currentChild](svg::DocumentReadAccess&, EntityHandle) {
      return currentChild.nextSibling();
    });
  }

  return result;
}

json EditorControlSession::selectedElementJson() const {
  if (!app_.selectedElement().has_value()) {
    return nullptr;
  }
  return ElementJson(*app_.selectedElement());
}

bool EditorControlSession::ensureDocumentLoaded(std::string* error) const {
  if (!app_.hasDocument()) {
    *error = "no SVG document loaded";
    return false;
  }
  return true;
}

bool EditorControlSession::waitUntilIdle(std::string* error) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (asyncRenderer_.isBusy() && std::chrono::steady_clock::now() < deadline) {
    (void)asyncRenderer_.pollResult();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (asyncRenderer_.pollResult().has_value()) {}
  if (asyncRenderer_.isBusy()) {
    *error = "timed out waiting for async renderer to go idle";
    return false;
  }
  return true;
}

}  // namespace donner::editor::mcp
