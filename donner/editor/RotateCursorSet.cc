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
#include "donner/svg/renderer/RendererTinySkia.h"

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
  std::string svg(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  std::ostringstream replacement;
  replacement << "rotate(" << RotationDegreesForCorner(corner) << ",16,16)";
  ReplaceFirst(&svg, kRotationPlaceholder, replacement.str());
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
    std::string_view svgSource, std::shared_ptr<geode::GeodeDevice> /*geodeDevice*/) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parseResult = svg::parser::SVGParser::ParseSVG(svgSource, warnings);
  if (parseResult.hasError()) {
    return std::nullopt;
  }

  svg::SVGDocument document = std::move(parseResult.result());
  // Cursor SVGs always need a CPU bitmap for GLFW. Rendering them through the
  // selected Geode backend would submit GPU work and synchronously read it back
  // during editor startup, before the first document frame. Keep this small,
  // fixed-size rasterization on TinySkia even when document rendering uses Geode.
  svg::RendererTinySkia renderer;
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
}

bool RotateCursorSet::setRotateCursor(SelectionTransformCorner corner) {
  if (!valid_ || window_ == nullptr) {
    return false;
  }

  GLFWcursor* cursor = rotateCursors_[CornerIndex(corner)];
  if (cursor == nullptr) {
    return false;
  }

  glfwSetCursor(window_, cursor);
  customCursorActive_ = true;
  return true;
}

bool RotateCursorSet::setScaleCursor(SelectionTransformCorner corner) {
  if (!valid_ || window_ == nullptr) {
    return false;
  }

  GLFWcursor* cursor = scaleCursors_[CornerIndex(corner)];
  if (cursor == nullptr) {
    return false;
  }

  glfwSetCursor(window_, cursor);
  customCursorActive_ = true;
  return true;
}

bool RotateCursorSet::setSelectCursor() {
  if (!valid_ || window_ == nullptr || selectCursor_ == nullptr) {
    return false;
  }

  glfwSetCursor(window_, selectCursor_);
  customCursorActive_ = true;
  return true;
}

bool RotateCursorSet::setPathModifyCursor() {
  if (!valid_ || window_ == nullptr || pathModifyCursor_ == nullptr) {
    return false;
  }

  glfwSetCursor(window_, pathModifyCursor_);
  customCursorActive_ = true;
  return true;
}

bool RotateCursorSet::setPanCursor(PanCursorKind kind) {
  if (!valid_ || window_ == nullptr) {
    return false;
  }

  GLFWcursor* cursor = panCursors_[PanCursorIndex(kind)];
  if (cursor == nullptr) {
    return false;
  }

  glfwSetCursor(window_, cursor);
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

  GLFWcursor* cursor = penCursors_[PenCursorIndex(hint)];
  if (cursor == nullptr) {
    return false;
  }

  glfwSetCursor(window_, cursor);
  customCursorActive_ = true;
  return true;
}

void RotateCursorSet::clearIfActive() {
  if (!customCursorActive_ || window_ == nullptr) {
    return;
  }

  glfwSetCursor(window_, nullptr);
  customCursorActive_ = false;
}

void RotateCursorSet::destroy() {
  clearIfActive();
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
  window_ = nullptr;
  valid_ = false;
}

}  // namespace donner::editor
