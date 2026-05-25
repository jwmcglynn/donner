#include "donner/editor/RotateCursorSet.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "GLFW/glfw3.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/editor/PanClosedCursorSvg.h"
#include "donner/editor/PanCursorSvg.h"
#include "donner/editor/PenCursorSvg.h"
#include "donner/editor/RotateCursorSvg.h"
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

double RotationDegreesForCorner(SelectionTransformCorner corner) {
  switch (corner) {
    case SelectionTransformCorner::TopLeft: return 0.0;
    case SelectionTransformCorner::TopRight: return 90.0;
    case SelectionTransformCorner::BottomRight: return 180.0;
    case SelectionTransformCorner::BottomLeft: return 270.0;
  }
  return 0.0;
}

std::string BuildRotateCursorSvg(SelectionTransformCorner corner) {
  std::string svg(reinterpret_cast<const char*>(embedded::kRotateCursorSvg.data()),
                  embedded::kRotateCursorSvg.size());
  std::ostringstream replacement;
  replacement << "rotate(" << RotationDegreesForCorner(corner) << ",16,16)";
  ReplaceFirst(&svg, kRotationPlaceholder, replacement.str());

  std::ostringstream rasterSize;
  rasterSize << kCursorRasterSizePx;
  const std::string rasterSizeText = rasterSize.str();
  ReplaceFirst(&svg, R"svg(width="32")svg", std::string("width=\"") + rasterSizeText + "\"");
  ReplaceFirst(&svg, R"svg(height="32")svg", std::string("height=\"") + rasterSizeText + "\"");
  return svg;
}

std::string BuildPanCursorSvg(PanCursorKind kind) {
  std::string svg;
  switch (kind) {
    case PanCursorKind::OpenHand:
      svg.assign(reinterpret_cast<const char*>(embedded::kPanCursorSvg.data()),
                 embedded::kPanCursorSvg.size());
      break;
    case PanCursorKind::ClosedHand:
      svg.assign(reinterpret_cast<const char*>(embedded::kPanClosedCursorSvg.data()),
                 embedded::kPanClosedCursorSvg.size());
      break;
  }

  std::ostringstream rasterSize;
  rasterSize << kCursorRasterSizePx;
  const std::string rasterSizeText = rasterSize.str();
  ReplaceFirst(&svg, R"svg(width="32")svg", std::string("width=\"") + rasterSizeText + "\"");
  ReplaceFirst(&svg, R"svg(height="32")svg", std::string("height=\"") + rasterSizeText + "\"");
  return svg;
}

std::string BuildPenCursorSvg() {
  std::string svg(reinterpret_cast<const char*>(embedded::kPenCursorSvg.data()),
                  embedded::kPenCursorSvg.size());

  std::ostringstream rasterSize;
  rasterSize << kCursorRasterSizePx;
  const std::string rasterSizeText = rasterSize.str();
  ReplaceFirst(&svg, R"svg(width="32")svg", std::string("width=\"") + rasterSizeText + "\"");
  ReplaceFirst(&svg, R"svg(height="32")svg", std::string("height=\"") + rasterSizeText + "\"");
  return svg;
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
  return RenderImageFromSvg(BuildRotateCursorSvg(corner), std::move(geodeDevice));
}

std::optional<RotateCursorImage> RenderPanCursorImage(
    PanCursorKind kind, std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  return RenderImageFromSvg(BuildPanCursorSvg(kind), std::move(geodeDevice));
}

std::optional<RotateCursorImage> RenderPenCursorImage(
    std::shared_ptr<geode::GeodeDevice> geodeDevice) {
  return RenderImageFromSvg(BuildPenCursorSvg(), std::move(geodeDevice));
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

  const std::array<SelectionTransformCorner, 4> corners = {
      SelectionTransformCorner::TopLeft,
      SelectionTransformCorner::TopRight,
      SelectionTransformCorner::BottomRight,
      SelectionTransformCorner::BottomLeft,
  };

  for (SelectionTransformCorner corner : corners) {
    std::optional<RotateCursorImage> image = RenderRotateCursorImage(corner, geodeDevice);
    if (!image.has_value()) {
      destroy();
      return false;
    }

    GLFWimage glfwImage{
        .width = image->width,
        .height = image->height,
        .pixels = image->rgba.data(),
    };
    GLFWcursor* cursor = glfwCreateCursor(&glfwImage, kCursorHotspotPx, kCursorHotspotPx);
    if (cursor == nullptr) {
      destroy();
      return false;
    }
    rotateCursors_[CornerIndex(corner)] = cursor;
  }

  for (PanCursorKind kind : {PanCursorKind::OpenHand, PanCursorKind::ClosedHand}) {
    std::optional<RotateCursorImage> panImage = RenderPanCursorImage(kind, geodeDevice);
    if (!panImage.has_value()) {
      destroy();
      return false;
    }

    GLFWimage glfwImage{
        .width = panImage->width,
        .height = panImage->height,
        .pixels = panImage->rgba.data(),
    };
    GLFWcursor* cursor = glfwCreateCursor(&glfwImage, kPanCursorHotspotPx, kPanCursorHotspotPx);
    if (cursor == nullptr) {
      destroy();
      return false;
    }
    panCursors_[PanCursorIndex(kind)] = cursor;
  }

  std::optional<RotateCursorImage> penImage = RenderPenCursorImage(geodeDevice);
  if (!penImage.has_value()) {
    destroy();
    return false;
  }

  GLFWimage glfwImage{
      .width = penImage->width,
      .height = penImage->height,
      .pixels = penImage->rgba.data(),
  };
  penCursor_ = glfwCreateCursor(&glfwImage, kPenCursorHotspotXPx, kPenCursorHotspotYPx);
  if (penCursor_ == nullptr) {
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
  if (!valid_ || window_ == nullptr || penCursor_ == nullptr) {
    return false;
  }

  glfwSetCursor(window_, penCursor_);
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
  for (GLFWcursor*& cursor : rotateCursors_) {
    if (cursor != nullptr) {
      glfwDestroyCursor(cursor);
      cursor = nullptr;
    }
  }
  for (GLFWcursor*& cursor : panCursors_) {
    if (cursor != nullptr) {
      glfwDestroyCursor(cursor);
      cursor = nullptr;
    }
  }
  if (penCursor_ != nullptr) {
    glfwDestroyCursor(penCursor_);
    penCursor_ = nullptr;
  }
  window_ = nullptr;
  valid_ = false;
}

}  // namespace donner::editor
