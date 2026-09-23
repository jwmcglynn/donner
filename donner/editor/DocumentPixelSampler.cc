#include "donner/editor/DocumentPixelSampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace donner::editor {
namespace {

bool ContainsHalfOpen(const Box2d& box, const Vector2d& point) {
  return std::isfinite(point.x) && std::isfinite(point.y) && point.x >= box.topLeft.x &&
         point.y >= box.topLeft.y && point.x < box.bottomRight.x && point.y < box.bottomRight.y;
}

std::uint8_t Unpremultiply(std::uint8_t channel, std::uint8_t alpha) {
  return static_cast<std::uint8_t>(
      std::min(255u, (static_cast<unsigned>(channel) * 255u + alpha / 2u) / alpha));
}

}  // namespace

bool CanCaptureDocumentPixelSize(const Vector2i& dimensions) noexcept {
  if (dimensions.x <= 0 || dimensions.y <= 0 || dimensions.x > ViewportState::kMaxCanvasDim ||
      dimensions.y > ViewportState::kMaxCanvasDim) {
    return false;
  }
  const std::size_t rowBytes = static_cast<std::size_t>(dimensions.x) * 4u;
  const std::size_t alignedRowBytes = (rowBytes + 255u) & ~std::size_t{255u};
  return alignedRowBytes <= kDocumentPixelCaptureMaxBytes / static_cast<std::size_t>(dimensions.y);
}

bool IsValidDocumentPixelBitmap(const svg::RendererBitmap& bitmap) noexcept {
  if (bitmap.dimensions.x <= 0 || bitmap.dimensions.y <= 0 ||
      bitmap.dimensions.x > ViewportState::kMaxCanvasDim ||
      bitmap.dimensions.y > ViewportState::kMaxCanvasDim) {
    return false;
  }
  const std::size_t width = static_cast<std::size_t>(bitmap.dimensions.x);
  const std::size_t height = static_cast<std::size_t>(bitmap.dimensions.y);
  const std::size_t minimumRowBytes = width * 4u;
  if (bitmap.rowBytes < minimumRowBytes ||
      bitmap.rowBytes > kDocumentPixelCaptureMaxBytes / height) {
    return false;
  }
  return bitmap.pixels.size() >= bitmap.rowBytes * height &&
         bitmap.pixels.size() <= kDocumentPixelCaptureMaxBytes;
}

std::optional<Vector2i> DocumentPixelIndexAtScreenPoint(const ViewportState& viewport,
                                                        const EditorRasterViewport& rasterViewport,
                                                        const Vector2d& screenPoint) noexcept {
  const Box2d paneRect = Box2d::FromXYWH(viewport.paneOrigin.x, viewport.paneOrigin.y,
                                         viewport.paneSize.x, viewport.paneSize.y);
  if (!ContainsHalfOpen(paneRect, screenPoint)) {
    return std::nullopt;
  }
  const Vector2d documentPoint = viewport.screenToDocument(screenPoint);
  if (!ContainsHalfOpen(viewport.documentViewBox, documentPoint) ||
      !ContainsHalfOpen(rasterViewport.documentRect, documentPoint)) {
    return std::nullopt;
  }
  const Vector2d rasterPoint = rasterViewport.outputFromDocument.transformPosition(documentPoint);
  if (!std::isfinite(rasterPoint.x) || !std::isfinite(rasterPoint.y) || rasterPoint.x < 0.0 ||
      rasterPoint.y < 0.0 || rasterPoint.x >= rasterViewport.outputSizePx.x ||
      rasterPoint.y >= rasterViewport.outputSizePx.y) {
    return std::nullopt;
  }
  return Vector2i(static_cast<int>(std::floor(rasterPoint.x)),
                  static_cast<int>(std::floor(rasterPoint.y)));
}

std::optional<css::RGBA> ReadDocumentPixel(const svg::RendererBitmap& bitmap,
                                           const Vector2i& pixel) noexcept {
  if (!IsValidDocumentPixelBitmap(bitmap) || pixel.x < 0 || pixel.y < 0 ||
      pixel.x >= bitmap.dimensions.x || pixel.y >= bitmap.dimensions.y) {
    return std::nullopt;
  }
  const std::size_t offset =
      static_cast<std::size_t>(pixel.y) * bitmap.rowBytes + static_cast<std::size_t>(pixel.x) * 4u;
  const std::uint8_t* rgba = bitmap.pixels.data() + offset;
  if (rgba[3] == 0) {
    return css::RGBA(0, 0, 0, 0);
  }
  if (bitmap.alphaType == svg::AlphaType::Unpremultiplied) {
    return css::RGBA(rgba[0], rgba[1], rgba[2], rgba[3]);
  }
  return css::RGBA(Unpremultiply(rgba[0], rgba[3]), Unpremultiply(rgba[1], rgba[3]),
                   Unpremultiply(rgba[2], rgba[3]), rgba[3]);
}

}  // namespace donner::editor
