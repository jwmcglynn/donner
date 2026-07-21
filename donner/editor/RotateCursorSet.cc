#include "donner/editor/RotateCursorSet.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#include "GLFW/glfw3.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/editor/PanClosedCursorSvg.h"
#include "donner/editor/PanCursorSvg.h"
#include "donner/editor/PathModifyCursorSvg.h"
#include "donner/editor/PenCursorSvg.h"
#include "donner/editor/RotateCursorSvg.h"
#include "donner/editor/ScaleCursorSvg.h"
#include "donner/editor/SelectCursorSvg.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor {

namespace {

constexpr int kCursorSizePx = 32;
constexpr int kCursorRasterScale = 4;
constexpr int kCursorRasterSizePx = kCursorSizePx * kCursorRasterScale;
constexpr int kCursorHotspotPx = 16;
constexpr int kPanCursorHotspotPx = 15;
constexpr int kPenCursorHotspotXPx = 4;
constexpr int kPenCursorHotspotYPx = 4;
constexpr std::string_view kRotationPlaceholder = "rotate(0,16,16)";

#if defined(__EMSCRIPTEN__)
// clang-format off: C++ formatting corrupts JavaScript operators and object literals.
EM_JS(bool, RegisterBrowserCursor,
      (int cursorId, int cornerIndex, const char* svgSource, int svgLength, int hotspotX,
       int hotspotY, const char* fallbackSource, int fallbackLength), {
  const bytes = HEAPU8.subarray(svgSource, svgSource + svgLength);
  let binary = "";
  const chunkSize = 0x8000;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    binary += String.fromCharCode.apply(
        null, bytes.subarray(offset, Math.min(offset + chunkSize, bytes.length)));
  }

  const fallback = UTF8ToString(fallbackSource, fallbackLength);
  const dataUrl = "data:image/svg+xml;base64," + btoa(binary);
  const svgCssValue = "url(\"" + dataUrl + "\") " + hotspotX + " " + hotspotY + ", " + fallback;
  const svgSupported = !globalThis.CSS || globalThis.CSS.supports("cursor", svgCssValue);
  const cssValue = svgSupported ? svgCssValue : fallback;

  let registry = Module["__donnerBrowserCursorRegistry"];
  if (!registry) {
    registry = {
      entries: new Map(),
      active: false,
      activeKey: null,
      previousInlineCursor: "",
      previousInlineCursorPriority: "",
    };
    Module["__donnerBrowserCursorRegistry"] = registry;
  }
  const key = cursorId + ":" + cornerIndex;
  registry.entries.set(key, {
    cssValue: cssValue,
    svgSupported: svgSupported,
    hotspotX: hotspotX,
    hotspotY: hotspotY,
    fallback: fallback,
  });

  let svgSupportedCount = 0;
  registry.entries.forEach(function(entry) {
    if (entry.svgSupported) {
      svgSupportedCount += 1;
    }
  });
  const diagnostics = globalThis["__donnerBrowserCursorStats"] || {
    applied: 0,
    applyRequests: 0,
    domMutations: 0,
    registered: 0,
    redundantApplySkips: 0,
    svgSupported: 0,
  };
  diagnostics.registered = registry.entries.size;
  diagnostics.svgSupported = svgSupportedCount;
  globalThis["__donnerBrowserCursorStats"] = diagnostics;
  return true;
});

EM_JS(bool, ApplyBrowserCursor, (int cursorId, int cornerIndex), {
  const registry = Module["__donnerBrowserCursorRegistry"];
  const canvas = Module["canvas"];
  if (!registry || !canvas) {
    return false;
  }

  const key = cursorId + ":" + cornerIndex;
  const entry = registry.entries.get(key);
  if (!entry) {
    return false;
  }
  const diagnostics = globalThis["__donnerBrowserCursorStats"];
  if (diagnostics) {
    diagnostics.applyRequests += 1;
  }
  if (registry.active && registry.activeKey === key) {
    if (diagnostics) {
      diagnostics.redundantApplySkips += 1;
    }
    return true;
  }
  if (!registry.active) {
    registry.previousInlineCursor = canvas.style.getPropertyValue("cursor");
    registry.previousInlineCursorPriority = canvas.style.getPropertyPriority("cursor");
  }
  canvas.style.setProperty("cursor", entry.cssValue);
  registry.active = true;
  registry.activeKey = key;
  if (diagnostics) {
    diagnostics.applied += 1;
    diagnostics.domMutations += 1;
    diagnostics.lastKey = key;
    diagnostics.lastHotspot = [entry.hotspotX, entry.hotspotY];
    diagnostics.lastFallback = entry.fallback;
    diagnostics.lastSvgSupported = entry.svgSupported;
  }
  return canvas.style.getPropertyValue("cursor") !== "";
});

EM_JS(void, ClearBrowserCursor, (), {
  const registry = Module["__donnerBrowserCursorRegistry"];
  const canvas = Module["canvas"];
  if (!registry || !registry.active || !canvas) {
    return;
  }

  if (registry.previousInlineCursor) {
    canvas.style.setProperty(
        "cursor", registry.previousInlineCursor, registry.previousInlineCursorPriority);
  } else {
    canvas.style.removeProperty("cursor");
  }
  registry.active = false;
  registry.activeKey = null;
  registry.previousInlineCursor = "";
  registry.previousInlineCursorPriority = "";
});

EM_JS(void, DestroyBrowserCursorRegistry, (), {
  const registry = Module["__donnerBrowserCursorRegistry"];
  if (!registry) {
    return;
  }

  const canvas = Module["canvas"];
  if (registry.active && canvas) {
    if (registry.previousInlineCursor) {
      canvas.style.setProperty(
          "cursor", registry.previousInlineCursor, registry.previousInlineCursorPriority);
    } else {
      canvas.style.removeProperty("cursor");
    }
  }
  delete Module["__donnerBrowserCursorRegistry"];
  delete globalThis["__donnerBrowserCursorStats"];
});
// clang-format on
#endif

bool ReplaceFirst(std::string* text, std::string_view needle, std::string_view replacement) {
  const std::size_t offset = text->find(needle);
  if (offset == std::string::npos) {
    return false;
  }

  text->replace(offset, needle.size(), replacement);
  return true;
}

std::size_t CornerIndex(SelectionTransformCorner corner) {
  switch (corner) {
    case SelectionTransformCorner::TopLeft: return 0;
    case SelectionTransformCorner::TopRight: return 1;
    case SelectionTransformCorner::BottomRight: return 2;
    case SelectionTransformCorner::BottomLeft: return 3;
  }
  return 0;
}

std::size_t PanCursorIndex(PanCursorKind kind) {
  switch (kind) {
    case PanCursorKind::OpenHand: return 0;
    case PanCursorKind::ClosedHand: return 1;
  }
  return 0;
}

std::size_t PenCursorIndex(PenCursorHint hint) {
  switch (hint) {
    case PenCursorHint::Base: return 0;
    case PenCursorHint::Add: return 1;
    case PenCursorHint::Remove: return 2;
    case PenCursorHint::Close: return 3;
  }
  return 0;
}

std::span<const unsigned char> PenCursorSvgBytes(PenCursorHint hint) {
  switch (hint) {
    case PenCursorHint::Base: return embedded::kPenCursorSvg;
    case PenCursorHint::Add: return embedded::kPenAddCursorSvg;
    case PenCursorHint::Remove: return embedded::kPenRemoveCursorSvg;
    case PenCursorHint::Close: return embedded::kPenCloseCursorSvg;
  }
  return embedded::kPenCursorSvg;
}

double RotationDegreesForCorner(SelectionTransformCorner corner) {
  switch (corner) {
    case SelectionTransformCorner::TopLeft: return 0.0;
    case SelectionTransformCorner::TopRight: return 90.0;
    case SelectionTransformCorner::BottomRight: return 180.0;
    case SelectionTransformCorner::BottomLeft: return 270.0;
  }
  return 0.0;
}

std::string RotatedCursorSvg(std::span<const unsigned char> bytes,
                             SelectionTransformCorner corner) {
  std::string svg(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  std::ostringstream replacement;
  replacement << "rotate(" << RotationDegreesForCorner(corner) << ",16,16)";
  ReplaceFirst(&svg, kRotationPlaceholder, replacement.str());
  return svg;
}

// Substitute the source SVG's declared 32px width/height for the 4x raster
// size, so Donner rasterizes the art at kCursorRasterSizePx before downsample.
void ApplyRasterSize(std::string* svg) {
  std::ostringstream rasterSize;
  rasterSize << kCursorRasterSizePx;
  const std::string rasterSizeText = rasterSize.str();
  ReplaceFirst(svg, R"svg(width="32")svg", std::string("width=\"") + rasterSizeText + "\"");
  ReplaceFirst(svg, R"svg(height="32")svg", std::string("height=\"") + rasterSizeText + "\"");
}

// Raster-sized SVG for a fixed-orientation cursor.
std::string SizedCursorSvg(std::span<const unsigned char> bytes) {
  std::string svg(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  ApplyRasterSize(&svg);
  return svg;
}

// Raster-sized SVG for a corner-oriented cursor: rewrites the
// `rotate(0,16,16)` placeholder on the glyph group to the corner's angle.
std::string SizedRotatedCursorSvg(std::span<const unsigned char> bytes,
                                  SelectionTransformCorner corner) {
  std::string svg = RotatedCursorSvg(bytes, corner);
  ApplyRasterSize(&svg);
  return svg;
}

std::span<const unsigned char> PanCursorSvgBytes(PanCursorKind kind) {
  switch (kind) {
    case PanCursorKind::OpenHand: return embedded::kPanCursorSvg;
    case PanCursorKind::ClosedHand: return embedded::kPanClosedCursorSvg;
  }
  return embedded::kPanCursorSvg;
}

#if defined(__EMSCRIPTEN__)
std::string BrowserCursorSvg(EditorCursor cursor, SelectionTransformCorner corner) {
  switch (cursor) {
    case EditorCursor::Select:
      return std::string(reinterpret_cast<const char*>(embedded::kSelectCursorSvg.data()),
                         embedded::kSelectCursorSvg.size());
    case EditorCursor::Pen:
    case EditorCursor::PenAdd:
    case EditorCursor::PenRemove:
    case EditorCursor::PenClose: {
      const std::span<const unsigned char> bytes =
          PenCursorSvgBytes(cursor == EditorCursor::Pen         ? PenCursorHint::Base
                            : cursor == EditorCursor::PenAdd    ? PenCursorHint::Add
                            : cursor == EditorCursor::PenRemove ? PenCursorHint::Remove
                                                                : PenCursorHint::Close);
      return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    case EditorCursor::Rotate: return RotatedCursorSvg(embedded::kRotateCursorSvg, corner);
    case EditorCursor::Scale: return RotatedCursorSvg(embedded::kScaleCursorSvg, corner);
    case EditorCursor::PathModify:
      return std::string(reinterpret_cast<const char*>(embedded::kPathModifyCursorSvg.data()),
                         embedded::kPathModifyCursorSvg.size());
    case EditorCursor::PanOpen:
    case EditorCursor::PanClosed: {
      const std::span<const unsigned char> bytes = PanCursorSvgBytes(
          cursor == EditorCursor::PanOpen ? PanCursorKind::OpenHand : PanCursorKind::ClosedHand);
      return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
  }
  return {};
}

std::string_view BrowserCursorFallback(EditorCursor cursor, SelectionTransformCorner corner) {
  switch (cursor) {
    case EditorCursor::Select: return "default";
    case EditorCursor::Pen:
    case EditorCursor::PenAdd:
    case EditorCursor::PenRemove:
    case EditorCursor::PenClose:
    case EditorCursor::PathModify: return "crosshair";
    case EditorCursor::Rotate: return "all-scroll";
    case EditorCursor::Scale:
      return corner == SelectionTransformCorner::TopLeft ||
                     corner == SelectionTransformCorner::BottomRight
                 ? "nwse-resize"
                 : "nesw-resize";
    case EditorCursor::PanOpen: return "grab";
    case EditorCursor::PanClosed: return "grabbing";
  }
  return "default";
}

bool RegisterBrowserCursorVariant(EditorCursor cursor, SelectionTransformCorner corner) {
  const std::string svg = BrowserCursorSvg(cursor, corner);
  const CursorHotspot hotspot = HotspotForCursor(cursor);
  const std::string_view fallback = BrowserCursorFallback(cursor, corner);
  return RegisterBrowserCursor(static_cast<int>(cursor), static_cast<int>(CornerIndex(corner)),
                               svg.data(), static_cast<int>(svg.size()), hotspot.x, hotspot.y,
                               fallback.data(), static_cast<int>(fallback.size()));
}

bool ApplyBrowserCursorVariant(EditorCursor cursor, SelectionTransformCorner corner) {
  return ApplyBrowserCursor(static_cast<int>(cursor), static_cast<int>(CornerIndex(corner)));
}
#endif

std::optional<std::vector<unsigned char>> DownsampleToStraightAlphaTightRgba(
    const svg::RendererBitmap& bitmap, int rasterScale) {
  if (bitmap.empty() || rasterScale <= 0 || bitmap.dimensions.x != kCursorSizePx * rasterScale ||
      bitmap.dimensions.y != kCursorSizePx * rasterScale ||
      bitmap.rowBytes < static_cast<std::size_t>(bitmap.dimensions.x) * 4u) {
    return std::nullopt;
  }

  std::vector<unsigned char> result(kCursorSizePx * kCursorSizePx * 4u);
  const int sourcePixelsPerOutputPixel = rasterScale * rasterScale;
  for (int y = 0; y < kCursorSizePx; ++y) {
    for (int x = 0; x < kCursorSizePx; ++x) {
      int premulR = 0;
      int premulG = 0;
      int premulB = 0;
      int alphaSum = 0;
      for (int dy = 0; dy < rasterScale; ++dy) {
        const int srcY = y * rasterScale + dy;
        const auto* srcRow =
            bitmap.pixels.data() + static_cast<std::size_t>(srcY) * bitmap.rowBytes;
        for (int dx = 0; dx < rasterScale; ++dx) {
          const int srcX = x * rasterScale + dx;
          const auto* src = srcRow + srcX * 4;
          const int alpha = src[3];
          alphaSum += alpha;
          if (bitmap.alphaType == svg::AlphaType::Premultiplied) {
            premulR += src[0];
            premulG += src[1];
            premulB += src[2];
          } else {
            premulR += (static_cast<int>(src[0]) * alpha + 127) / 255;
            premulG += (static_cast<int>(src[1]) * alpha + 127) / 255;
            premulB += (static_cast<int>(src[2]) * alpha + 127) / 255;
          }
        }
      }

      const int alpha = (alphaSum + sourcePixelsPerOutputPixel / 2) / sourcePixelsPerOutputPixel;
      const std::size_t dstOffset = (static_cast<std::size_t>(y) * kCursorSizePx + x) * 4u;
      if (alpha > 0) {
        const int avgPremulR =
            (premulR + sourcePixelsPerOutputPixel / 2) / sourcePixelsPerOutputPixel;
        const int avgPremulG =
            (premulG + sourcePixelsPerOutputPixel / 2) / sourcePixelsPerOutputPixel;
        const int avgPremulB =
            (premulB + sourcePixelsPerOutputPixel / 2) / sourcePixelsPerOutputPixel;
        result[dstOffset + 0] =
            static_cast<unsigned char>(std::clamp((avgPremulR * 255 + alpha / 2) / alpha, 0, 255));
        result[dstOffset + 1] =
            static_cast<unsigned char>(std::clamp((avgPremulG * 255 + alpha / 2) / alpha, 0, 255));
        result[dstOffset + 2] =
            static_cast<unsigned char>(std::clamp((avgPremulB * 255 + alpha / 2) / alpha, 0, 255));
      }
      result[dstOffset + 3] = static_cast<unsigned char>(std::clamp(alpha, 0, 255));
    }
  }

  return result;
}

std::optional<RotateCursorImage> RenderImageFromSvg(
    std::string_view svgSource, std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parseResult = svg::parser::SVGParser::ParseSVG(svgSource, warnings);
  if (parseResult.hasError()) {
    return std::nullopt;
  }

  svg::SVGDocument document = std::move(parseResult.result());
  svg::Renderer renderer(std::move(geodeDevice));
  renderer.draw(document);
  svg::RendererBitmap bitmap = renderer.takeSnapshot();
  std::optional<std::vector<unsigned char>> rgba =
      DownsampleToStraightAlphaTightRgba(bitmap, kCursorRasterScale);
  if (!rgba.has_value()) {
    return std::nullopt;
  }

  return RotateCursorImage{
      .width = kCursorSizePx,
      .height = kCursorSizePx,
      .rgba = std::move(*rgba),
  };
}

}  // namespace

std::optional<RotateCursorImage> RenderRotateCursorImage(
    SelectionTransformCorner corner, std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  return RenderImageFromSvg(SizedRotatedCursorSvg(embedded::kRotateCursorSvg, corner),
                            std::move(geodeDevice));
}

std::optional<RotateCursorImage> RenderScaleCursorImage(
    SelectionTransformCorner corner, std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  return RenderImageFromSvg(SizedRotatedCursorSvg(embedded::kScaleCursorSvg, corner),
                            std::move(geodeDevice));
}

std::optional<RotateCursorImage> RenderSelectCursorImage(
    std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  return RenderImageFromSvg(SizedCursorSvg(embedded::kSelectCursorSvg), std::move(geodeDevice));
}

std::optional<RotateCursorImage> RenderPathModifyCursorImage(
    std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  return RenderImageFromSvg(SizedCursorSvg(embedded::kPathModifyCursorSvg), std::move(geodeDevice));
}

std::optional<RotateCursorImage> RenderPanCursorImage(
    PanCursorKind kind, std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  return RenderImageFromSvg(SizedCursorSvg(PanCursorSvgBytes(kind)), std::move(geodeDevice));
}

std::optional<RotateCursorImage> RenderPenCursorImage(
    std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  return RenderPenCursorImage(PenCursorHint::Base, std::move(geodeDevice));
}

std::optional<RotateCursorImage> RenderPenCursorImage(
    PenCursorHint hint, std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  return RenderImageFromSvg(SizedCursorSvg(PenCursorSvgBytes(hint)), std::move(geodeDevice));
}

CursorHotspot HotspotForCursor(EditorCursor cursor) {
  switch (cursor) {
    case EditorCursor::Select: return CursorHotspot{5, 4};
    case EditorCursor::Pen:
    case EditorCursor::PenAdd:
    case EditorCursor::PenRemove:
    case EditorCursor::PenClose: return CursorHotspot{4, 4};
    case EditorCursor::Rotate:
    case EditorCursor::Scale: return CursorHotspot{16, 16};
    case EditorCursor::PathModify: return CursorHotspot{6, 6};
    case EditorCursor::PanOpen:
    case EditorCursor::PanClosed: return CursorHotspot{15, 15};
  }
  return CursorHotspot{0, 0};
}

bool CursorUsesCorner(EditorCursor cursor) {
  return cursor == EditorCursor::Rotate || cursor == EditorCursor::Scale;
}

std::optional<RotateCursorImage> RenderEditorCursorImage(
    EditorCursor cursor, SelectionTransformCorner corner,
    std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  switch (cursor) {
    case EditorCursor::Select: return RenderSelectCursorImage(std::move(geodeDevice));
    case EditorCursor::Pen:
      return RenderPenCursorImage(PenCursorHint::Base, std::move(geodeDevice));
    case EditorCursor::PenAdd:
      return RenderPenCursorImage(PenCursorHint::Add, std::move(geodeDevice));
    case EditorCursor::PenRemove:
      return RenderPenCursorImage(PenCursorHint::Remove, std::move(geodeDevice));
    case EditorCursor::PenClose:
      return RenderPenCursorImage(PenCursorHint::Close, std::move(geodeDevice));
    case EditorCursor::Rotate: return RenderRotateCursorImage(corner, std::move(geodeDevice));
    case EditorCursor::Scale: return RenderScaleCursorImage(corner, std::move(geodeDevice));
    case EditorCursor::PathModify: return RenderPathModifyCursorImage(std::move(geodeDevice));
    case EditorCursor::PanOpen:
      return RenderPanCursorImage(PanCursorKind::OpenHand, std::move(geodeDevice));
    case EditorCursor::PanClosed:
      return RenderPanCursorImage(PanCursorKind::ClosedHand, std::move(geodeDevice));
  }
  return std::nullopt;
}

RotateCursorSet::~RotateCursorSet() {
  destroy();
}

bool RotateCursorSet::initialize(GLFWwindow* window,
                                 std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  destroy();
  window_ = window;
  if (window_ == nullptr) {
    return false;
  }

#ifdef __EMSCRIPTEN__
  (void)geodeDevice;
  for (EditorCursor cursor : kEditorCursors) {
    if (CursorUsesCorner(cursor)) {
      for (SelectionTransformCorner corner :
           {SelectionTransformCorner::TopLeft, SelectionTransformCorner::TopRight,
            SelectionTransformCorner::BottomRight, SelectionTransformCorner::BottomLeft}) {
        if (!RegisterBrowserCursorVariant(cursor, corner)) {
          destroy();
          return false;
        }
      }
    } else if (!RegisterBrowserCursorVariant(cursor, SelectionTransformCorner::TopLeft)) {
      destroy();
      return false;
    }
  }

  valid_ = true;
  return true;
#else
  // Render an image and create a GLFW cursor with the given hotspot, storing it
  // in @p slot. Returns false (and leaves the caller to `destroy()`) on any
  // failure, so a partial cursor set never goes live.
  const auto createCursor = [&](std::optional<RotateCursorImage> image, int hotspotX, int hotspotY,
                                GLFWcursor*& slot) -> bool {
    if (!image.has_value()) {
      return false;
    }
    GLFWimage glfwImage{
        .width = image->width,
        .height = image->height,
        .pixels = image->rgba.data(),
    };
    GLFWcursor* cursor = glfwCreateCursor(&glfwImage, hotspotX, hotspotY);
    if (cursor == nullptr) {
      return false;
    }
    slot = cursor;
    return true;
  };

  const std::array<SelectionTransformCorner, 4> corners = {
      SelectionTransformCorner::TopLeft,
      SelectionTransformCorner::TopRight,
      SelectionTransformCorner::BottomRight,
      SelectionTransformCorner::BottomLeft,
  };

  for (SelectionTransformCorner corner : corners) {
    if (!createCursor(RenderRotateCursorImage(corner, geodeDevice), kCursorHotspotPx,
                      kCursorHotspotPx, rotateCursors_[CornerIndex(corner)]) ||
        !createCursor(RenderScaleCursorImage(corner, geodeDevice), kCursorHotspotPx,
                      kCursorHotspotPx, scaleCursors_[CornerIndex(corner)])) {
      destroy();
      return false;
    }
  }

  for (PanCursorKind kind : {PanCursorKind::OpenHand, PanCursorKind::ClosedHand}) {
    if (!createCursor(RenderPanCursorImage(kind, geodeDevice), kPanCursorHotspotPx,
                      kPanCursorHotspotPx, panCursors_[PanCursorIndex(kind)])) {
      destroy();
      return false;
    }
  }

  for (PenCursorHint hint :
       {PenCursorHint::Base, PenCursorHint::Add, PenCursorHint::Remove, PenCursorHint::Close}) {
    if (!createCursor(RenderPenCursorImage(hint, geodeDevice), kPenCursorHotspotXPx,
                      kPenCursorHotspotYPx, penCursors_[PenCursorIndex(hint)])) {
      destroy();
      return false;
    }
  }

  const CursorHotspot selectHotspot = HotspotForCursor(EditorCursor::Select);
  const CursorHotspot pathModifyHotspot = HotspotForCursor(EditorCursor::PathModify);
  if (!createCursor(RenderSelectCursorImage(geodeDevice), selectHotspot.x, selectHotspot.y,
                    selectCursor_) ||
      !createCursor(RenderPathModifyCursorImage(geodeDevice), pathModifyHotspot.x,
                    pathModifyHotspot.y, pathModifyCursor_)) {
    destroy();
    return false;
  }

  valid_ = true;
  return true;
#endif
}

bool RotateCursorSet::setRotateCursor(SelectionTransformCorner corner) {
  if (!valid_ || window_ == nullptr) {
    return false;
  }

#if defined(__EMSCRIPTEN__)
  if (!ApplyBrowserCursorVariant(EditorCursor::Rotate, corner)) {
    return false;
  }
#else
  GLFWcursor* cursor = rotateCursors_[CornerIndex(corner)];
  if (cursor == nullptr) {
    return false;
  }

  glfwSetCursor(window_, cursor);
#endif
  customCursorActive_ = true;
  return true;
}

bool RotateCursorSet::setScaleCursor(SelectionTransformCorner corner) {
  if (!valid_ || window_ == nullptr) {
    return false;
  }

#if defined(__EMSCRIPTEN__)
  if (!ApplyBrowserCursorVariant(EditorCursor::Scale, corner)) {
    return false;
  }
#else
  GLFWcursor* cursor = scaleCursors_[CornerIndex(corner)];
  if (cursor == nullptr) {
    return false;
  }

  glfwSetCursor(window_, cursor);
#endif
  customCursorActive_ = true;
  return true;
}

bool RotateCursorSet::setSelectCursor() {
  if (!valid_ || window_ == nullptr) {
    return false;
  }

#if defined(__EMSCRIPTEN__)
  if (!ApplyBrowserCursorVariant(EditorCursor::Select, SelectionTransformCorner::TopLeft)) {
    return false;
  }
#else
  if (selectCursor_ == nullptr) {
    return false;
  }
  glfwSetCursor(window_, selectCursor_);
#endif
  customCursorActive_ = true;
  return true;
}

bool RotateCursorSet::setPathModifyCursor() {
  if (!valid_ || window_ == nullptr) {
    return false;
  }

#if defined(__EMSCRIPTEN__)
  if (!ApplyBrowserCursorVariant(EditorCursor::PathModify, SelectionTransformCorner::TopLeft)) {
    return false;
  }
#else
  if (pathModifyCursor_ == nullptr) {
    return false;
  }
  glfwSetCursor(window_, pathModifyCursor_);
#endif
  customCursorActive_ = true;
  return true;
}

bool RotateCursorSet::setPanCursor(PanCursorKind kind) {
  if (!valid_ || window_ == nullptr) {
    return false;
  }

#if defined(__EMSCRIPTEN__)
  const EditorCursor cursorId =
      kind == PanCursorKind::OpenHand ? EditorCursor::PanOpen : EditorCursor::PanClosed;
  if (!ApplyBrowserCursorVariant(cursorId, SelectionTransformCorner::TopLeft)) {
    return false;
  }
#else
  GLFWcursor* cursor = panCursors_[PanCursorIndex(kind)];
  if (cursor == nullptr) {
    return false;
  }

  glfwSetCursor(window_, cursor);
#endif
  customCursorActive_ = true;
  return true;
}

bool RotateCursorSet::setPenCursor() {
  return setPenCursor(PenCursorHint::Base);
}

bool RotateCursorSet::setPenCursor(PenCursorHint hint) {
  if (!valid_ || window_ == nullptr) {
    return false;
  }

#if defined(__EMSCRIPTEN__)
  const EditorCursor cursorId = hint == PenCursorHint::Base     ? EditorCursor::Pen
                                : hint == PenCursorHint::Add    ? EditorCursor::PenAdd
                                : hint == PenCursorHint::Remove ? EditorCursor::PenRemove
                                                                : EditorCursor::PenClose;
  if (!ApplyBrowserCursorVariant(cursorId, SelectionTransformCorner::TopLeft)) {
    return false;
  }
#else
  GLFWcursor* cursor = penCursors_[PenCursorIndex(hint)];
  if (cursor == nullptr) {
    return false;
  }

  glfwSetCursor(window_, cursor);
#endif
  customCursorActive_ = true;
  return true;
}

void RotateCursorSet::clearIfActive() {
  if (!customCursorActive_ || window_ == nullptr) {
    return;
  }

#if defined(__EMSCRIPTEN__)
  ClearBrowserCursor();
#else
  glfwSetCursor(window_, nullptr);
#endif
  customCursorActive_ = false;
}

void RotateCursorSet::destroy() {
  clearIfActive();
#if defined(__EMSCRIPTEN__)
  DestroyBrowserCursorRegistry();
#else
  const auto destroyOne = [](GLFWcursor*& cursor) {
    if (cursor != nullptr) {
      glfwDestroyCursor(cursor);
      cursor = nullptr;
    }
  };
  for (GLFWcursor*& cursor : rotateCursors_) {
    destroyOne(cursor);
  }
  for (GLFWcursor*& cursor : scaleCursors_) {
    destroyOne(cursor);
  }
  for (GLFWcursor*& cursor : panCursors_) {
    destroyOne(cursor);
  }
  for (GLFWcursor*& cursor : penCursors_) {
    destroyOne(cursor);
  }
  destroyOne(selectCursor_);
  destroyOne(pathModifyCursor_);
#endif
  window_ = nullptr;
  valid_ = false;
}

}  // namespace donner::editor
