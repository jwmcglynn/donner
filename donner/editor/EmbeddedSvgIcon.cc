#include "donner/editor/EmbeddedSvgIcon.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "donner/base/Length.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor {
namespace {

std::string_view StringViewFromSpan(std::span<const unsigned char> bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void NormalizeIconBitmapToTintableAlphaMask(svg::RendererBitmap* bitmap) {
  if (bitmap == nullptr || bitmap->empty()) {
    return;
  }

  for (int y = 0; y < bitmap->dimensions.y; ++y) {
    unsigned char* row = bitmap->pixels.data() + static_cast<std::size_t>(y) * bitmap->rowBytes;
    for (int x = 0; x < bitmap->dimensions.x; ++x) {
      unsigned char* pixel = row + static_cast<std::size_t>(x) * 4u;
      const unsigned char alpha = pixel[3];
      pixel[0] = alpha;
      pixel[1] = alpha;
      pixel[2] = alpha;
    }
  }
  bitmap->alphaType = svg::AlphaType::Premultiplied;
}

/// One shared renderer for all embedded-icon rasterization, created on first
/// use. Icons render lazily from UI-thread panel code only. Constructing a
/// fresh renderer per icon is disproportionately expensive on GPU backends -
/// each construction stands up a full WebGPU instance/adapter/device - and
/// showed up as a stream of duplicate "[Geode/wgpu-native] Adapter:" logs at
/// editor startup.
struct SharedIconRendererState {
  svg::RendererInterface* configuredRenderer = nullptr;
  std::unique_ptr<svg::Renderer> fallbackRenderer;
};

SharedIconRendererState& SharedIconRendererStateInstance() {
  static SharedIconRendererState state;
  return state;
}

svg::RendererInterface& SharedIconRenderer() {
  SharedIconRendererState& state = SharedIconRendererStateInstance();
  if (state.configuredRenderer != nullptr) {
    return *state.configuredRenderer;
  }
  if (state.fallbackRenderer == nullptr) {
    state.fallbackRenderer = std::make_unique<svg::Renderer>();
  }
  return *state.fallbackRenderer;
}

/// Parse an embedded icon and size its root viewport to the requested square.
std::optional<svg::SVGDocument> ParseEmbeddedSvgIcon(std::span<const unsigned char> svgBytes,
                                                     int outputSizePx) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parseResult = svg::parser::SVGParser::ParseSVG(StringViewFromSpan(svgBytes), warnings);
  if (parseResult.hasError()) {
    return std::nullopt;
  }

  svg::SVGDocument document = std::move(parseResult.result());
  svg::SVGSVGElement root = document.svgElement();
  root.setWidth(Lengthd(outputSizePx, Lengthd::Unit::Px));
  root.setHeight(Lengthd(outputSizePx, Lengthd::Unit::Px));
  return document;
}

/// Identifies a prewarmed rasterization. Embedded resources are static arrays,
/// so the source pointer and length identify the asset without hashing bytes.
struct PrewarmedIconKey {
  const unsigned char* data = nullptr;
  std::size_t size = 0;
  int outputSizePx = 0;
  bool tintableMask = true;

  bool operator==(const PrewarmedIconKey&) const = default;
};

struct PrewarmedIconKeyHash {
  std::size_t operator()(const PrewarmedIconKey& key) const {
    std::size_t hash = std::hash<const void*>()(key.data);
    hash = hash * 31u + std::hash<std::size_t>()(key.size);
    hash = hash * 31u + std::hash<int>()(key.outputSizePx);
    return hash * 31u + (key.tintableMask ? 1u : 0u);
  }
};

PrewarmedIconKey KeyForRequest(const EmbeddedSvgIconRequest& request) {
  return PrewarmedIconKey{request.svgBytes.data(), request.svgBytes.size(), request.outputSizePx,
                          request.tintableMask};
}

using PrewarmedIconMap =
    std::unordered_map<PrewarmedIconKey, svg::RendererBitmap, PrewarmedIconKeyHash>;

PrewarmedIconMap& PrewarmedIcons() {
  static PrewarmedIconMap icons;
  return icons;
}

/// Maximum atlas row width in device pixels. Icons are small (16-80 px), so a
/// shelf this wide keeps the atlas to a couple of rows while staying far below
/// any backend's maximum texture dimension.
constexpr int kIconAtlasMaxWidthPx = 1024;

/// Icons rasterized per atlas pass, which bounds peak memory during prewarm.
///
/// A pass has to hold every one of its documents alive until its frame ends:
/// the GPU backend keeps each document's resident geometry and bind groups in
/// components on that document's own registry and consumes them when the frame
/// is submitted (see `donner/svg/renderer/geode/GeodeResidentPathComponent.h`),
/// so releasing a document early draws blank tiles. Peak heap therefore scales
/// with the pass size, and the cost per in-flight document is large: prewarming
/// the editor's fourteen boot icons in one pass measured a 101.5 MB Wasm boot
/// heap high-water, against under 32 MB when they are rasterized a few at a
/// time. On Safari that difference is fatal rather than merely wasteful,
/// because the boot heap has to fit in `INITIAL_MEMORY` - growing the shared
/// memory traps a pooled pthread.
///
/// Each pass costs one GPU-to-CPU readback, the latency the atlas exists to
/// amortize, so this wants to be as large as the memory budget allows rather
/// than as small as possible. Re-measure the boot high-water before raising it.
constexpr std::size_t kIconsPerAtlasPass = 4;

/// Copy one square tile out of the atlas into a tightly-packed bitmap.
svg::RendererBitmap SliceAtlasTile(const svg::RendererBitmap& atlas, Vector2i originPx,
                                   int sizePx) {
  svg::RendererBitmap tile;
  tile.dimensions = Vector2i(sizePx, sizePx);
  tile.rowBytes = static_cast<std::size_t>(sizePx) * 4u;
  tile.alphaType = atlas.alphaType;
  tile.pixels.resize(tile.rowBytes * static_cast<std::size_t>(sizePx));

  for (int y = 0; y < sizePx; ++y) {
    const unsigned char* source = atlas.pixels.data() +
                                  static_cast<std::size_t>(originPx.y + y) * atlas.rowBytes +
                                  static_cast<std::size_t>(originPx.x) * 4u;
    unsigned char* destination = tile.pixels.data() + static_cast<std::size_t>(y) * tile.rowBytes;
    std::copy_n(source, tile.rowBytes, destination);
  }
  return tile;
}

std::optional<svg::RendererBitmap> RenderEmbeddedSvgBitmap(std::span<const unsigned char> svgBytes,
                                                           int outputSizePx,
                                                           bool normalizeToTintableMask) {
  if (outputSizePx <= 0) {
    return std::nullopt;
  }

  const PrewarmedIconMap& prewarmed = PrewarmedIcons();
  if (const auto it = prewarmed.find(PrewarmedIconKey{svgBytes.data(), svgBytes.size(),
                                                      outputSizePx, normalizeToTintableMask});
      it != prewarmed.end()) {
    return it->second;
  }

  std::optional<svg::SVGDocument> document = ParseEmbeddedSvgIcon(svgBytes, outputSizePx);
  if (!document.has_value()) {
    return std::nullopt;
  }

  svg::RendererInterface& renderer = SharedIconRenderer();
  renderer.draw(*document);
  svg::RendererBitmap bitmap = renderer.takeSnapshot();
  if (bitmap.empty()) {
    return std::nullopt;
  }

  if (normalizeToTintableMask) {
    NormalizeIconBitmapToTintableAlphaMask(&bitmap);
  }
  return bitmap;
}

/// Rasterize one atlas pass over `requests[firstIndex, lastIndex)` and write the
/// sliced tiles into `results`. Every document in the pass stays alive until
/// the pass returns; see `kIconsPerAtlasPass`.
void RenderEmbeddedSvgIconAtlasPass(std::span<const EmbeddedSvgIconRequest> requests,
                                    std::size_t firstIndex, std::size_t lastIndex,
                                    std::vector<std::optional<svg::RendererBitmap>>& results) {
  struct AtlasSlot {
    std::size_t requestIndex = 0;
    Vector2i originPx = Vector2i::Zero();
    int sizePx = 0;
  };

  // Documents are kept alive (and pointer-stable) until the atlas pass is done.
  std::vector<svg::SVGDocument> documents;
  std::vector<AtlasSlot> slots;
  const std::size_t passSize = lastIndex - firstIndex;
  documents.reserve(passSize);
  slots.reserve(passSize);

  // Shelf packing: fill a row left to right, wrap when the next tile would
  // exceed the row width. Tiles are square, so a row is as tall as its largest.
  int cursorX = 0;
  int cursorY = 0;
  int rowHeightPx = 0;
  int atlasWidthPx = 0;
  for (std::size_t i = firstIndex; i < lastIndex; ++i) {
    const EmbeddedSvgIconRequest& request = requests[i];
    if (request.outputSizePx <= 0) {
      continue;
    }

    std::optional<svg::SVGDocument> document =
        ParseEmbeddedSvgIcon(request.svgBytes, request.outputSizePx);
    if (!document.has_value()) {
      continue;
    }

    if (cursorX > 0 && cursorX + request.outputSizePx > kIconAtlasMaxWidthPx) {
      cursorY += rowHeightPx;
      cursorX = 0;
      rowHeightPx = 0;
    }

    slots.push_back(AtlasSlot{i, Vector2i(cursorX, cursorY), request.outputSizePx});
    documents.push_back(std::move(*document));

    cursorX += request.outputSizePx;
    rowHeightPx = std::max(rowHeightPx, request.outputSizePx);
    atlasWidthPx = std::max(atlasWidthPx, cursorX);
  }

  if (slots.empty()) {
    return;
  }

  std::vector<svg::AtlasDocumentPlacement> placements;
  placements.reserve(slots.size());
  for (std::size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
    placements.push_back(
        svg::AtlasDocumentPlacement{&documents[slotIndex], slots[slotIndex].originPx});
  }

  const Vector2i atlasSizePx(atlasWidthPx, cursorY + rowHeightPx);
  const svg::RendererBitmap atlas =
      svg::RenderDocumentsToAtlasBitmap(SharedIconRenderer(), placements, atlasSizePx);
  // A backend that produced a smaller target than asked for cannot be sliced by
  // the layout that was planned against the requested size. Fall back to the
  // per-icon path rather than reading past the rows that came back.
  if (atlas.dimensions.x < atlasSizePx.x || atlas.dimensions.y < atlasSizePx.y) {
    return;
  }

  for (const AtlasSlot& slot : slots) {
    svg::RendererBitmap tile = SliceAtlasTile(atlas, slot.originPx, slot.sizePx);
    if (requests[slot.requestIndex].tintableMask) {
      NormalizeIconBitmapToTintableAlphaMask(&tile);
    }
    results[slot.requestIndex] = std::move(tile);
  }
}

}  // namespace

void ConfigureEmbeddedSvgIconRenderer(svg::RendererInterface& renderer) {
  SharedIconRendererState& state = SharedIconRendererStateInstance();
  if (state.configuredRenderer == &renderer) {
    return;
  }
  state.fallbackRenderer.reset();
  state.configuredRenderer = &renderer;
}

void ResetEmbeddedSvgIconRenderer(const svg::RendererInterface& expectedRenderer) {
  SharedIconRendererState& state = SharedIconRendererStateInstance();
  if (state.configuredRenderer == &expectedRenderer) {
    state.configuredRenderer = nullptr;
  }
}

std::optional<svg::RendererBitmap> RenderEmbeddedSvgIcon(std::span<const unsigned char> svgBytes,
                                                         int outputSizePx) {
  return RenderEmbeddedSvgBitmap(svgBytes, outputSizePx, /*normalizeToTintableMask=*/true);
}

std::optional<svg::RendererBitmap> RenderEmbeddedSvgArtwork(std::span<const unsigned char> svgBytes,
                                                            int outputSizePx) {
  return RenderEmbeddedSvgBitmap(svgBytes, outputSizePx, /*normalizeToTintableMask=*/false);
}

std::vector<std::optional<svg::RendererBitmap>> RenderEmbeddedSvgIconBatch(
    std::span<const EmbeddedSvgIconRequest> requests) {
  std::vector<std::optional<svg::RendererBitmap>> results(requests.size());
  for (std::size_t firstIndex = 0; firstIndex < requests.size();
       firstIndex += kIconsPerAtlasPass) {
    RenderEmbeddedSvgIconAtlasPass(
        requests, firstIndex, std::min(firstIndex + kIconsPerAtlasPass, requests.size()), results);
  }
  return results;
}

void PrewarmEmbeddedSvgIcons(std::span<const EmbeddedSvgIconRequest> requests) {
  PrewarmedIconMap& prewarmed = PrewarmedIcons();

  // Distinct rasterizations only: several affordances share one asset (the
  // subtract-front and subtract-back buttons draw the same Bootstrap glyph),
  // and an already-prewarmed icon needs no second tile.
  std::vector<EmbeddedSvgIconRequest> uniqueRequests;
  std::vector<PrewarmedIconKey> uniqueKeys;
  uniqueRequests.reserve(requests.size());
  uniqueKeys.reserve(requests.size());
  for (const EmbeddedSvgIconRequest& request : requests) {
    const PrewarmedIconKey key = KeyForRequest(request);
    if (prewarmed.contains(key) ||
        std::find(uniqueKeys.begin(), uniqueKeys.end(), key) != uniqueKeys.end()) {
      continue;
    }
    uniqueKeys.push_back(key);
    uniqueRequests.push_back(request);
  }

  std::vector<std::optional<svg::RendererBitmap>> bitmaps =
      RenderEmbeddedSvgIconBatch(uniqueRequests);
  for (std::size_t i = 0; i < bitmaps.size(); ++i) {
    if (bitmaps[i].has_value()) {
      prewarmed.insert_or_assign(uniqueKeys[i], std::move(*bitmaps[i]));
    }
  }
}

}  // namespace donner::editor
