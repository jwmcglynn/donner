// Port of Mask::fill_path and Mask::intersect_path from mask.rs.
// Lives in the painter target to avoid a circular dependency between
// tiny_skia_core and tiny_skia_scan.

#include "tiny_skia/Color.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Math.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/pipeline/Blitter.h"
#include "tiny_skia/scan/Path.h"
#include "tiny_skia/scan/PathAa.h"

namespace tiny_skia {

namespace {

// Fill into a temporary RGBA pixmap and extract the alpha channel back to mask data.
// The RasterPipelineBlitter operates on RGBA (4 bytes per pixel), but Mask stores
// 1 byte per pixel.  Wrapping the mask buffer directly would cause a buffer overflow.
void fillMaskRegion(const Path& path, FillRule fillRule, bool antiAlias,
                    const ScreenIntRect& clipRect, std::uint8_t* maskData, std::size_t maskStride,
                    std::uint32_t regionWidth, std::uint32_t regionHeight) {
  // Create a temporary RGBA pixmap for the fill.
  auto tempPixmap = Pixmap::fromSize(regionWidth, regionHeight);
  if (!tempPixmap) {
    return;
  }
  auto tempMut = tempPixmap->mutableView();
  auto subpix = tempMut.subpixmap();

  auto blitter = pipeline::RasterPipelineBlitter::createMask(&subpix);
  if (!blitter.has_value()) {
    return;
  }

  if (antiAlias) {
    scan::path_aa::fillPath(path, fillRule, clipRect, *blitter);
  } else {
    scan::fillPath(path, fillRule, clipRect, *blitter);
  }

  // Extract alpha channel from RGBA back to mask (1 byte per pixel).
  const auto* rgba = tempPixmap->data().data();
  for (std::uint32_t y = 0; y < regionHeight; ++y) {
    for (std::uint32_t x = 0; x < regionWidth; ++x) {
      const auto srcOffset = (y * regionWidth + x) * 4;
      const auto dstOffset = y * maskStride + x;
      maskData[dstOffset] = rgba[srcOffset + 3];
    }
  }
}

}  // namespace

void Mask::fillPath(const Path& path, FillRule fillRule, bool antiAlias, Transform transform) {
  if (transform.isIdentity()) {
    // Skip empty paths and horizontal/vertical lines.
    const auto pathBounds = path.bounds();
    if (isNearlyZero(pathBounds.width()) || isNearlyZero(pathBounds.height())) {
      return;
    }

    if (detail::isTooBigForMath(path)) {
      return;
    }

    if (auto tiler = detail::DrawTiler::create(width(), height())) {
      auto pathCopy = path;

      while (auto tile = tiler->next()) {
        const auto ts = Transform::fromTranslate(-static_cast<float>(tile->x()),
                                                 -static_cast<float>(tile->y()));
        auto transformed = pathCopy.transform(ts);
        if (!transformed.has_value()) {
          return;
        }
        pathCopy = std::move(*transformed);

        const auto clipRect = tile->size().toScreenIntRect(0, 0);
        auto subpix = subpixmap(tile->toIntRect());
        if (!subpix.has_value()) {
          continue;
        }

        fillMaskRegion(pathCopy, fillRule, antiAlias, clipRect, subpix->data, subpix->realWidth,
                       subpix->size.width(), subpix->size.height());

        const auto tsBack =
            Transform::fromTranslate(static_cast<float>(tile->x()), static_cast<float>(tile->y()));
        auto untransformed = pathCopy.transform(tsBack);
        if (!untransformed.has_value()) {
          return;
        }
        pathCopy = std::move(*untransformed);
      }
    } else {
      const auto clipRect = size_.toScreenIntRect(0, 0);
      fillMaskRegion(path, fillRule, antiAlias, clipRect, data_.data(), width(), width(), height());
    }
  } else {
    auto transformed = path.transform(transform);
    if (!transformed.has_value()) {
      return;
    }
    fillPath(*transformed, fillRule, antiAlias, Transform::identity());
  }
}

void Mask::intersectPath(const Path& path, FillRule fillRule, bool antiAlias, Transform transform) {
  auto submask = Mask::fromSize(width(), height());
  if (!submask.has_value()) {
    return;
  }
  submask->fillPath(path, fillRule, antiAlias, transform);

  for (std::size_t i = 0; i < data_.size(); ++i) {
    data_[i] = premultiplyU8(data_[i], submask->data_[i]);
  }
}

}  // namespace tiny_skia
