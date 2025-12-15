#include "donner/backends/tiny_skia_cpp/Painter.h"

#include "donner/backends/tiny_skia_cpp/PathGeometry.h"
#include "donner/backends/tiny_skia_cpp/Shader.h"

namespace donner::backends::tiny_skia_cpp {

namespace {

Expected<std::monostate, std::string> BlendMask(Mask& mask, const Mask* clipMask, Pixmap& pixmap,
                                                const PaintContext& paintContext) {
  if (clipMask != nullptr) {
    if (!clipMask->isValid() || clipMask->width() != mask.width() ||
        clipMask->height() != mask.height()) {
      return Expected<std::monostate, std::string>::Failure("Clip mask is invalid or mis-sized");
    }

    for (int y = 0; y < mask.height(); ++y) {
      uint8_t* row = mask.data() + static_cast<size_t>(y) * mask.strideBytes();
      const uint8_t* clipRow = clipMask->data() + static_cast<size_t>(y) * clipMask->strideBytes();
      for (int x = 0; x < mask.width(); ++x) {
        const uint8_t clipped =
            static_cast<uint8_t>((static_cast<uint16_t>(row[x]) * clipRow[x] + 127) / 255);
        row[x] = clipped;
      }
    }
  }

  for (int y = 0; y < mask.height(); ++y) {
    const uint8_t* row = mask.data() + static_cast<size_t>(y) * mask.strideBytes();
    BlendMaskSpan(pixmap, 0, y, row, mask.width(), paintContext);
  }

  return Expected<std::monostate, std::string>::Success(std::monostate{});
}

}  // namespace

Expected<std::monostate, std::string> FillPath(const svg::PathSpline& spline, const Paint& paint,
                                               Pixmap& pixmap, FillRule fillRule,
                                               const Transform& transform, const Mask* clipMask) {
  if (!pixmap.isValid()) {
    return Expected<std::monostate, std::string>::Failure("Destination pixmap is invalid");
  }

  auto paintContext = PaintContext::Create(paint);
  if (!paintContext.hasValue()) {
    return Expected<std::monostate, std::string>::Failure(paintContext.error());
  }

  Mask mask = RasterizeFill(spline, pixmap.width(), pixmap.height(), fillRule, paint.antiAlias,
                            transform);
  if (!mask.isValid()) {
    return Expected<std::monostate, std::string>::Failure("Failed to rasterize path");
  }

  return BlendMask(mask, clipMask, pixmap, paintContext.value());
}

Expected<std::monostate, std::string> StrokePath(const svg::PathSpline& spline, const Stroke& stroke,
                                                 const Paint& paint, Pixmap& pixmap,
                                                 const Transform& transform, const Mask* clipMask) {
  if (!pixmap.isValid()) {
    return Expected<std::monostate, std::string>::Failure("Destination pixmap is invalid");
  }

  auto paintContext = PaintContext::Create(paint);
  if (!paintContext.hasValue()) {
    return Expected<std::monostate, std::string>::Failure(paintContext.error());
  }

  const svg::PathSpline outline = ApplyStroke(spline, stroke);
  Mask mask = RasterizeFill(outline, pixmap.width(), pixmap.height(), FillRule::kNonZero,
                            paint.antiAlias, transform);
  if (!mask.isValid()) {
    return Expected<std::monostate, std::string>::Failure("Failed to rasterize stroke");
  }

  return BlendMask(mask, clipMask, pixmap, paintContext.value());
}

Expected<std::monostate, std::string> DrawPixmap(int x, int y, const Pixmap& source,
                                                 const PixmapPaint& paint, Pixmap& pixmap,
                                                 const Transform& transform, const Mask* clipMask) {
  if (!pixmap.isValid()) {
    return Expected<std::monostate, std::string>::Failure("Destination pixmap is invalid");
  }
  if (!source.isValid()) {
    return Expected<std::monostate, std::string>::Failure("Source pixmap is invalid");
  }

  const Transform translation =
      Transform::Translate(static_cast<double>(x), static_cast<double>(y));
  const Transform patternTransform = transform * translation;
  auto pattern = Shader::MakePattern(source, SpreadMode::kPad, paint.quality, paint.opacity,
                                     patternTransform);
  if (!pattern.hasValue()) {
    return Expected<std::monostate, std::string>::Failure(pattern.error());
  }

  Paint fillPaint;
  fillPaint.blendMode = paint.blendMode;
  fillPaint.opacity = 1.0f;
  fillPaint.antiAlias = false;
  fillPaint.shader = pattern.value();

  svg::PathSpline rect;
  rect.moveTo({static_cast<double>(x), static_cast<double>(y)});
  rect.lineTo({static_cast<double>(x + source.width()), static_cast<double>(y)});
  rect.lineTo({static_cast<double>(x + source.width()), static_cast<double>(y + source.height())});
  rect.lineTo({static_cast<double>(x), static_cast<double>(y + source.height())});
  rect.closePath();

  return FillPath(rect, fillPaint, pixmap, FillRule::kNonZero, transform, clipMask);
}

}  // namespace donner::backends::tiny_skia_cpp
