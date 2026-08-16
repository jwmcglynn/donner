#include "donner/editor/OverlayRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorTheme.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/core/Display.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"

namespace donner::editor {

namespace {

/// Desired on-screen stroke thickness for selection chrome (path outlines
/// and AABBs), in logical UI pixels. It is multiplied by the viewport
/// device-pixel ratio before conversion into document/world units, so the
/// overlay reads at the same visual weight on Retina and non-Retina displays.
constexpr double kSelectionStrokeLogicalPixels = 1.25;

/// Desired on-screen stroke thickness for source-hover chrome, in logical UI pixels.
constexpr double kHoverStrokeLogicalPixels = 1.5;

/// Marquee stroke thickness, in logical UI pixels.
constexpr double kMarqueeStrokeLogicalPixels = 1.5;

/// Selected path anchor square size, in logical UI pixels.
constexpr double kPathAnchorLogicalPixels = 5.0;

/// Selected path control-point square size, in logical UI pixels.
constexpr double kPathControlPointLogicalPixels = 4.0;

svg::PaintParams MakeSelectionStrokePaint(double worldStrokeWidth, double opacity = 1.0) {
  svg::PaintParams paint;
  // Accent-tinted selection stroke, no fill (EditorTheme selection token).
  paint.stroke = svg::PaintServer::Solid(css::Color(EditorTheme::Active().selectionRgba(0xff)));
  paint.fill = svg::PaintServer::None{};
  paint.strokeOpacity = opacity;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Butt;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Miter;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

svg::PaintParams MakeTextSelectionFillPaint() {
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid(css::Color(EditorTheme::Active().selectionRgba(0x55)));
  paint.stroke = svg::PaintServer::None{};
  paint.fillOpacity = 1.0;
  return paint;
}

svg::PaintParams MakePathControlLinePaint(double worldStrokeWidth) {
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::None{};
  paint.stroke = svg::PaintServer::Solid(css::Color(EditorTheme::Active().selectionRgba(0xa0)));
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Round;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Round;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

svg::PaintParams MakeDisplayNoneSelectionStrokePaint(double worldStrokeWidth) {
  svg::PaintParams paint = MakeSelectionStrokePaint(worldStrokeWidth);
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0x5f, 0x9a, 0xb2, 0xff)));
  return paint;
}

svg::PaintParams MakeHandlePaint(double worldStrokeWidth, double opacity = 1.0) {
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid(css::Color(css::RGBA(0xff, 0xff, 0xff, 0xff)));
  paint.stroke = svg::PaintServer::Solid(css::Color(EditorTheme::Active().selectionRgba(0xff)));
  paint.fillOpacity = opacity;
  paint.strokeOpacity = opacity;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Butt;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Miter;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

svg::PaintParams MakePathPointPaint() {
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid(css::Color(EditorTheme::Active().selectionRgba(0xff)));
  paint.stroke = svg::PaintServer::None{};
  paint.fillOpacity = 1.0;
  return paint;
}

svg::PaintParams MakeSourceHoverShapePaint(double worldStrokeWidth) {
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid(css::Color(EditorTheme::Active().selectionRgba(0x30)));
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0xff, 0xff, 0xff, 0xd0)));
  paint.fillOpacity = 1.0;
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Round;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Round;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

svg::PaintParams MakeSourceHoverBoundsPaint(double worldStrokeWidth) {
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::None{};
  paint.stroke = svg::PaintServer::Solid(css::Color(EditorTheme::Active().selectionRgba(0xc8)));
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Round;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Round;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

/// Desired on-screen stroke thickness for the locked-rejection flash outline.
/// Slightly heavier than the selection stroke so the rejection reads clearly.
constexpr double kLockedFlashStrokeLogicalPixels = 2.0;

/// Red stroke, no fill, for the "this element is locked, you can't select it" flash. The stroke's
/// alpha is the flash `intensity` (1 → 0 as it fades) scaled into the 0-255 channel range.
svg::PaintParams MakeLockedFlashStrokePaint(double worldStrokeWidth, float intensity) {
  svg::PaintParams paint;
  const float clampedIntensity = std::clamp(intensity, 0.0f, 1.0f);
  const uint8_t alpha = static_cast<uint8_t>(std::lround(clampedIntensity * 255.0f));
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0xff, 0x1a, 0x1a, alpha)));
  paint.fill = svg::PaintServer::None{};
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Round;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Round;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

/// Translucent accent fill, no stroke - used for the marquee fill pass.
/// Alpha 0x33 keeps the prior marquee weight, now tinted from the EditorTheme
/// selection token instead of the hard-coded cyan.
svg::PaintParams MakeMarqueeFillPaint() {
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid(css::Color(EditorTheme::Active().selectionRgba(0x33)));
  paint.stroke = svg::PaintServer::None{};
  paint.fillOpacity = 1.0;
  return paint;
}

/// Solid white stroke, no fill - the marquee's outer outline. Matches
/// the prior `IM_COL32(0xff, 0xff, 0xff, 0xff)` in `RenderPanePresenter`.
svg::PaintParams MakeMarqueeStrokePaint(double worldStrokeWidth) {
  svg::PaintParams paint;
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0xff, 0xff, 0xff, 0xff)));
  paint.fill = svg::PaintServer::None{};
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Butt;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Miter;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

/// Linear scale factor baked into `canvasFromDoc`. Both axes are
/// identical under `preserveAspectRatio="xMid* meet|slice"`, which is
/// the only case the editor renders today.
double LinearScale(const Transform2d& canvasFromDoc) {
  return canvasFromDoc.transformVector(Vector2d(1.0, 0.0)).length();
}

double DevicePixelsForLogicalPixels(double logicalPixels, double devicePixelRatio) {
  return logicalPixels * std::max(devicePixelRatio, 1.0);
}

Box2d PointBoxForDevicePixels(const Vector2d& centerDoc, double sizeDevicePixels,
                              double canvasScale) {
  const double sizeDoc = canvasScale > 1e-9 ? sizeDevicePixels / canvasScale : sizeDevicePixels;
  const Vector2d halfSize(sizeDoc * 0.5, sizeDoc * 0.5);
  return Box2d(centerDoc - halfSize, centerDoc + halfSize);
}

/// Chrome sizes resolved against the transform the chrome is being drawn with.
/// Every value is a logical-pixel constant converted into document units, so
/// the same snapshot drawn at a different zoom keeps its on-screen size.
struct ChromeDrawScale {
  double canvasScale = 1.0;
  double selectionStrokeWidthWorld = 0.0;
  double hoverStrokeWidthWorld = 0.0;
  double marqueeStrokeWidthWorld = 0.0;
  double pathAnchorSizeDevicePixels = 0.0;
  double pathControlPointSizeDevicePixels = 0.0;
};

ChromeDrawScale ChromeDrawScaleFor(const Transform2d& canvasFromDoc, double devicePixelRatio) {
  const double scale = LinearScale(canvasFromDoc);
  const auto pixelToWorld = [scale](double pixels) {
    return scale > 1e-9 ? pixels / scale : pixels;
  };
  return ChromeDrawScale{
      .canvasScale = scale,
      .selectionStrokeWidthWorld = pixelToWorld(
          DevicePixelsForLogicalPixels(kSelectionStrokeLogicalPixels, devicePixelRatio)),
      .hoverStrokeWidthWorld =
          pixelToWorld(DevicePixelsForLogicalPixels(kHoverStrokeLogicalPixels, devicePixelRatio)),
      .marqueeStrokeWidthWorld =
          pixelToWorld(DevicePixelsForLogicalPixels(kMarqueeStrokeLogicalPixels, devicePixelRatio)),
      .pathAnchorSizeDevicePixels =
          DevicePixelsForLogicalPixels(kPathAnchorLogicalPixels, devicePixelRatio),
      .pathControlPointSizeDevicePixels =
          DevicePixelsForLogicalPixels(kPathControlPointLogicalPixels, devicePixelRatio),
  };
}

std::array<Vector2d, 4> TransformedBoxCorners(const Box2d& box,
                                              const Transform2d& documentFromBoxDocument) {
  const std::array<Vector2d, 4> corners{
      box.topLeft,
      Vector2d(box.bottomRight.x, box.topLeft.y),
      box.bottomRight,
      Vector2d(box.topLeft.x, box.bottomRight.y),
  };

  std::array<Vector2d, 4> transformed;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    transformed[i] = documentFromBoxDocument.transformPosition(corners[i]);
  }
  return transformed;
}

Box2d HandleBoxForCorner(const Vector2d& cornerDoc, double scale) {
  const SelectionTransformHandleBoxes handleBoxes =
      SelectionTransformHandleBoxesForBounds(Box2d(cornerDoc, cornerDoc), scale);
  return handleBoxes.boxes.front();
}

Path PathForCorners(const std::array<Vector2d, 4>& corners) {
  PathBuilder builder;
  builder.moveTo(corners[0]);
  builder.lineTo(corners[1]);
  builder.lineTo(corners[2]);
  builder.lineTo(corners[3]);
  builder.closePath();
  return builder.build();
}

Path TransformPath(const Path& path, const Transform2d& destFromSource) {
  PathBuilder builder;
  path.forEach([&](Path::Verb verb, std::span<const Vector2d> points) {
    switch (verb) {
      case Path::Verb::MoveTo: builder.moveTo(destFromSource.transformPosition(points[0])); break;
      case Path::Verb::LineTo: builder.lineTo(destFromSource.transformPosition(points[0])); break;
      case Path::Verb::QuadTo:
        builder.quadTo(destFromSource.transformPosition(points[0]),
                       destFromSource.transformPosition(points[1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(destFromSource.transformPosition(points[0]),
                        destFromSource.transformPosition(points[1]),
                        destFromSource.transformPosition(points[2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  });
  return builder.build();
}

Path TransformPathToDocument(const Path& path, const Transform2d& documentFromElement) {
  return TransformPath(path, documentFromElement);
}

bool ElementDisplayNone(const svg::SVGElement& element) {
  return element.getComputedStyle().display.get().value() == svg::Display::None;
}

bool HasDisplayNoneInAncestorChain(const svg::SVGElement& element) {
  svg::SVGElement current = element;
  while (true) {
    if (ElementDisplayNone(current)) {
      return true;
    }

    std::optional<svg::SVGElement> parent = current.parentElement();
    if (!parent.has_value()) {
      return false;
    }
    current = *parent;
  }
}

bool BoxesIntersect(const Box2d& lhs, const Box2d& rhs) {
  return lhs.bottomRight.x >= rhs.topLeft.x && lhs.topLeft.x <= rhs.bottomRight.x &&
         lhs.bottomRight.y >= rhs.topLeft.y && lhs.topLeft.y <= rhs.bottomRight.y;
}

bool BoxIntersectsCullRect(const Box2d& box, const std::optional<Box2d>& cullRectDoc) {
  return !cullRectDoc.has_value() || BoxesIntersect(box, *cullRectDoc);
}

bool ControlLineIntersectsCullRect(const SelectionChromeSnapshot::PathControlLine& lineDoc,
                                   const std::optional<Box2d>& cullRectDoc) {
  if (!cullRectDoc.has_value()) {
    return true;
  }

  Box2d lineBounds = Box2d::CreateEmpty(lineDoc.anchorDoc);
  lineBounds.addPoint(lineDoc.controlDoc);
  return BoxesIntersect(lineBounds, *cullRectDoc);
}

void AddBoxToOptional(std::optional<Box2d>* target, const Box2d& box) {
  if (target->has_value()) {
    (*target)->addBox(box);
  } else {
    *target = box;
  }
}

void AppendPathPointChrome(
    const Path& pathDoc, double canvasScale, double devicePixelRatio,
    const std::optional<Box2d>& cullRectDoc, std::vector<Vector2d>* outPathAnchorPoints,
    std::vector<SelectionChromeSnapshot::PathControlLine>* outPathControlLines,
    std::vector<Vector2d>* outPathControlPoints) {
  const double anchorSizeDevicePixels =
      DevicePixelsForLogicalPixels(kPathAnchorLogicalPixels, devicePixelRatio);
  const double controlPointSizeDevicePixels =
      DevicePixelsForLogicalPixels(kPathControlPointLogicalPixels, devicePixelRatio);

  // Culling uses the capture-time square footprint; the drawn square is sized
  // from the draw transform, but the two differ only by the zoom delta between
  // capture and draw, which is far below the cull rect's screen-space margin.
  auto appendPoint = [&](std::vector<Vector2d>* points, const Vector2d& pointDoc,
                         double sizeDevicePixels) {
    if (points == nullptr) {
      return;
    }

    const Box2d box = PointBoxForDevicePixels(pointDoc, sizeDevicePixels, canvasScale);
    if (BoxIntersectsCullRect(box, cullRectDoc)) {
      points->push_back(pointDoc);
    }
  };

  auto appendControlLine = [&](const Vector2d& anchorDoc, const Vector2d& controlDoc) {
    if (outPathControlLines == nullptr) {
      return;
    }

    const SelectionChromeSnapshot::PathControlLine line{
        .anchorDoc = anchorDoc,
        .controlDoc = controlDoc,
    };
    if (ControlLineIntersectsCullRect(line, cullRectDoc)) {
      outPathControlLines->push_back(line);
    }
  };

  Vector2d currentPointDoc;
  Vector2d subpathStartDoc;
  bool hasCurrentPoint = false;
  bool hasSubpathStart = false;
  pathDoc.forEach([&](Path::Verb verb, std::span<const Vector2d> points) {
    switch (verb) {
      case Path::Verb::MoveTo:
        currentPointDoc = points[0];
        subpathStartDoc = points[0];
        hasCurrentPoint = true;
        hasSubpathStart = true;
        appendPoint(outPathAnchorPoints, points[0], anchorSizeDevicePixels);
        break;
      case Path::Verb::LineTo:
        appendPoint(outPathAnchorPoints, points[0], anchorSizeDevicePixels);
        currentPointDoc = points[0];
        hasCurrentPoint = true;
        break;
      case Path::Verb::QuadTo:
        if (hasCurrentPoint) {
          appendControlLine(currentPointDoc, points[0]);
          appendControlLine(points[1], points[0]);
        }
        appendPoint(outPathControlPoints, points[0], controlPointSizeDevicePixels);
        appendPoint(outPathAnchorPoints, points[1], anchorSizeDevicePixels);
        currentPointDoc = points[1];
        hasCurrentPoint = true;
        break;
      case Path::Verb::CurveTo:
        if (hasCurrentPoint) {
          appendControlLine(currentPointDoc, points[0]);
          appendControlLine(points[2], points[1]);
        }
        appendPoint(outPathControlPoints, points[0], controlPointSizeDevicePixels);
        appendPoint(outPathControlPoints, points[1], controlPointSizeDevicePixels);
        appendPoint(outPathAnchorPoints, points[2], anchorSizeDevicePixels);
        currentPointDoc = points[2];
        hasCurrentPoint = true;
        break;
      case Path::Verb::ClosePath:
        if (hasSubpathStart) {
          currentPointDoc = subpathStartDoc;
          hasCurrentPoint = true;
        }
        break;
    }
  });
}

struct AppendChromeItemsOptions {
  bool includePaths = true;
  bool includePerElementAabbs = true;
  bool includePathPointChrome = true;
  double canvasScale = 1.0;
  double devicePixelRatio = 1.0;
  Transform2d representedDocumentFromLiveDocument = Transform2d();
};

/// Append one document-space baseline segment per laid-out line of @p text:
/// consecutive characters sharing a text-local baseline y form a line, and the
/// segment spans from the first glyph's pen position to the last glyph's
/// advance end on that baseline.
void AppendTextBaselines(const svg::SVGTextElement& text,
                         const Transform2d& representedDocumentFromLiveDocument,
                         const std::optional<Box2d>& cullRectDoc,
                         std::vector<SelectionChromeSnapshot::TextBaseline>* outTextBaselines) {
  const long charCount = text.getNumberOfChars();
  if (charCount <= 0) {
    return;
  }

  const Transform2d documentFromText = text.elementFromWorld();
  const auto appendBaseline = [&](const Vector2d& startLocal, const Vector2d& endLocal) {
    SelectionChromeSnapshot::TextBaseline baseline;
    baseline.startDoc = representedDocumentFromLiveDocument.transformPosition(
        documentFromText.transformPosition(startLocal));
    baseline.endDoc = representedDocumentFromLiveDocument.transformPosition(
        documentFromText.transformPosition(endLocal));
    Box2d segmentBounds = Box2d::CreateEmpty(baseline.startDoc);
    segmentBounds.addPoint(baseline.endDoc);
    if (!BoxIntersectsCullRect(segmentBounds, cullRectDoc)) {
      return;
    }
    outTextBaselines->push_back(baseline);
  };

  // Two lines are never closer than a fraction of the glyph size, so a small
  // absolute tolerance on the baseline y is enough to group a line's
  // characters without merging adjacent lines.
  constexpr double kBaselineYToleranceLocal = 0.25;
  std::optional<double> runBaselineY;
  double runStartX = 0.0;
  double runEndX = 0.0;
  for (long i = 0; i < charCount; ++i) {
    const Vector2d startLocal = text.getStartPositionOfChar(static_cast<std::size_t>(i));
    const Vector2d endLocal = text.getEndPositionOfChar(static_cast<std::size_t>(i));
    if (!runBaselineY.has_value() ||
        std::abs(startLocal.y - *runBaselineY) > kBaselineYToleranceLocal) {
      if (runBaselineY.has_value()) {
        appendBaseline(Vector2d(runStartX, *runBaselineY), Vector2d(runEndX, *runBaselineY));
      }
      runBaselineY = startLocal.y;
      runStartX = std::min(startLocal.x, endLocal.x);
      runEndX = std::max(startLocal.x, endLocal.x);
    } else {
      runStartX = std::min(runStartX, std::min(startLocal.x, endLocal.x));
      runEndX = std::max(runEndX, std::max(startLocal.x, endLocal.x));
    }
  }
  if (runBaselineY.has_value()) {
    appendBaseline(Vector2d(runStartX, *runBaselineY), Vector2d(runEndX, *runBaselineY));
  }
}

std::optional<Box2d> AppendChromeItems(
    std::span<const svg::SVGElement> elements, const std::optional<Box2d>& cullRectDoc,
    std::vector<SelectionChromeSnapshot::PathItem>* outPaths, std::vector<Box2d>* outAabbs,
    std::vector<Vector2d>* outPathAnchorPoints,
    std::vector<SelectionChromeSnapshot::PathControlLine>* outPathControlLines,
    std::vector<Vector2d>* outPathControlPoints,
    std::vector<SelectionChromeSnapshot::TextBaseline>* outTextBaselines = nullptr,
    AppendChromeItemsOptions options = {}) {
  std::optional<Box2d> combinedBounds;
  for (const auto& element : elements) {
    element.withWriteAccess([&element, &cullRectDoc, outPaths, outAabbs, outPathAnchorPoints,
                             outPathControlLines, outPathControlPoints, outTextBaselines, options,
                             &combinedBounds](svg::DocumentWriteAccess&, EntityHandle) {
      std::optional<Box2d> mergedBounds;
      for (const auto& geometry : CollectRenderableGeometry(element)) {
        const std::optional<Box2d> worldBoundsDoc = GeometryWorldFrameBounds(geometry);
        const std::optional<Box2d> representedBoundsDoc =
            worldBoundsDoc.has_value()
                ? std::make_optional(
                      options.representedDocumentFromLiveDocument.transformBox(*worldBoundsDoc))
                : std::nullopt;
        if (representedBoundsDoc.has_value()) {
          AddBoxToOptional(&mergedBounds, *representedBoundsDoc);
        }

        if (representedBoundsDoc.has_value() &&
            !BoxIntersectsCullRect(*representedBoundsDoc, cullRectDoc)) {
          continue;
        }

        if (options.includePaths) {
          const auto spline = geometry.computedSpline();
          if (!spline.has_value()) {
            continue;
          }

          SelectionChromeSnapshot::PathItem item;
          const Transform2d documentFromElement = geometry.elementFromWorld();
          item.pathDoc = TransformPath(TransformPathToDocument(*spline, documentFromElement),
                                       options.representedDocumentFromLiveDocument);
          item.displayNone = HasDisplayNoneInAncestorChain(geometry);
          if (options.includePathPointChrome && geometry.isa<svg::SVGPathElement>()) {
            AppendPathPointChrome(item.pathDoc, options.canvasScale, options.devicePixelRatio,
                                  cullRectDoc, outPathAnchorPoints, outPathControlLines,
                                  outPathControlPoints);
          }
          outPaths->push_back(std::move(item));
        }
      }

      // Text roots have no spline outline; they contribute their frame - the
      // authored text box for box text, or the full laid-out bounds for point
      // text - so text gets the same selection rectangle + transform handles
      // as shapes, plus a baseline underlay segment per line.
      for (const auto& text : CollectRenderableTextRoots(element)) {
        const std::optional<Box2d> frameBoundsDoc = TextWorldFrameBounds(text);
        if (!frameBoundsDoc.has_value()) {
          continue;
        }
        const Box2d representedFrameBoundsDoc =
            options.representedDocumentFromLiveDocument.transformBox(*frameBoundsDoc);
        AddBoxToOptional(&mergedBounds, representedFrameBoundsDoc);

        if (outTextBaselines != nullptr &&
            BoxIntersectsCullRect(representedFrameBoundsDoc, cullRectDoc)) {
          AppendTextBaselines(text, options.representedDocumentFromLiveDocument, cullRectDoc,
                              outTextBaselines);
        }
      }

      if (mergedBounds.has_value() && BoxIntersectsCullRect(*mergedBounds, cullRectDoc)) {
        AddBoxToOptional(&combinedBounds, *mergedBounds);
        if (options.includePerElementAabbs) {
          outAabbs->push_back(*mergedBounds);
        }
      }
    });
  }
  return combinedBounds;
}

/// Resolve a computed paint into a solid RGBA color for the live-path
/// preview, or nullopt-in-nullopt when the paint is `none`. Returns an empty
/// outer optional when the paint cannot be represented as a flat color
/// (gradient/pattern reference, context paint, currentColor).
std::optional<std::optional<css::RGBA>> SolidPreviewColor(const svg::PaintServer& paint) {
  if (paint.is<svg::PaintServer::None>()) {
    return std::optional<css::RGBA>(std::nullopt);
  }
  if (paint.is<svg::PaintServer::Solid>()) {
    const css::Color color = paint.get<svg::PaintServer::Solid>().color;
    if (!color.hasRGBA()) {
      return std::nullopt;
    }
    return std::optional<css::RGBA>(color.rgba());
  }
  return std::nullopt;
}

/// Capture the Pen tool's live-geometry preview for `element`: its post-flush
/// document-space path plus resolved solid paint. Returns nullopt when the
/// element is not plain solid-painted geometry (gradient/pattern paint,
/// currentColor, non-absolute stroke-width, or a filter/clip-path/mask/marker
/// that the preview could not reproduce) - the presenter then falls back to
/// the composited raster.
std::optional<SelectionChromeSnapshot::LivePathPreview> CaptureLivePathPreview(
    const svg::SVGElement& element, const Transform2d& representedDocumentFromLiveDocument) {
  return element.withWriteAccess([&element, &representedDocumentFromLiveDocument](
                                     svg::DocumentWriteAccess&, EntityHandle)
                                     -> std::optional<SelectionChromeSnapshot::LivePathPreview> {
    if (!element.isa<svg::SVGGeometryElement>()) {
      return std::nullopt;
    }
    const svg::SVGGeometryElement geometry = element.cast<svg::SVGGeometryElement>();
    const std::optional<Path> spline = geometry.computedSpline();
    if (!spline.has_value() || spline->empty()) {
      return std::nullopt;
    }

    const svg::PropertyRegistry& style = element.getComputedStyle();
    const auto filterValue = style.filter.get();
    if (style.clipPath.get().has_value() || style.mask.get().has_value() ||
        (filterValue.has_value() && !filterValue->empty()) || style.markerStart.get().has_value() ||
        style.markerMid.get().has_value() || style.markerEnd.get().has_value()) {
      return std::nullopt;
    }

    const std::optional<std::optional<css::RGBA>> fillColor =
        SolidPreviewColor(style.fill.get().value_or(svg::PaintServer::None{}));
    const std::optional<std::optional<css::RGBA>> strokeColor =
        SolidPreviewColor(style.stroke.get().value_or(svg::PaintServer::None{}));
    if (!fillColor.has_value() || !strokeColor.has_value()) {
      return std::nullopt;
    }

    const Lengthd strokeWidth = style.strokeWidth.get().value_or(Lengthd(1.0));
    if (strokeWidth.unit != Lengthd::Unit::None && strokeWidth.unit != Lengthd::Unit::Px) {
      return std::nullopt;
    }

    SelectionChromeSnapshot::LivePathPreview preview;
    preview.entity = element.unsafeEntityHandle().entity();
    preview.pathDoc = TransformPath(TransformPathToDocument(*spline, geometry.elementFromWorld()),
                                    representedDocumentFromLiveDocument);
    preview.fillRule = style.fillRule.get().value_or(FillRule::NonZero);
    preview.fillColor = *fillColor;
    preview.strokeColor = *strokeColor;
    preview.strokeWidthDoc = strokeWidth.value;
    preview.opacity = style.opacity.get().value_or(1.0);
    preview.fillOpacity = style.fillOpacity.get().value_or(1.0);
    preview.strokeOpacity = style.strokeOpacity.get().value_or(1.0);
    return preview;
  });
}

/// Drop handle anchors whose capture-time square footprint misses the cull rect.
void CullHandleAnchorsInPlace(std::vector<Vector2d>* anchors, double canvasScale,
                              const std::optional<Box2d>& cullRectDoc) {
  if (!cullRectDoc.has_value()) {
    return;
  }

  std::erase_if(*anchors, [&](const Vector2d& anchorDoc) {
    return !BoxesIntersect(HandleBoxForCorner(anchorDoc, canvasScale), *cullRectDoc);
  });
}

/// The four corner points of @p bounds, in the handle order the chrome draws.
std::vector<Vector2d> SelectionTransformCornerPoints(const Box2d& boundsDoc) {
  return {
      boundsDoc.topLeft,
      Vector2d(boundsDoc.bottomRight.x, boundsDoc.topLeft.y),
      boundsDoc.bottomRight,
      Vector2d(boundsDoc.topLeft.x, boundsDoc.bottomRight.y),
  };
}

std::optional<SelectionChromeSnapshot::OrientedBox> CullOrientedBox(
    const SelectionChromeSnapshot::OrientedBox& orientedBox,
    const std::optional<Box2d>& cullRectDoc) {
  if (!cullRectDoc.has_value()) {
    return orientedBox;
  }

  Box2d bounds = Box2d::CreateEmpty(orientedBox.cornersDoc.front());
  for (const Vector2d& corner : orientedBox.cornersDoc) {
    bounds.addPoint(corner);
  }

  if (!BoxesIntersect(bounds, *cullRectDoc)) {
    return std::nullopt;
  }
  return orientedBox;
}

}  // namespace

void OverlayRenderer::drawChrome(svg::RendererInterface& renderer, const EditorApp& editor) {
  if (!editor.hasDocument()) {
    return;
  }

  // Map from document (viewBox) coordinates into canvas pixels. The
  // document has already been rendered with this transform baked into
  // every draw call; we have to apply the same one to the overlay or
  // the chrome drifts off the content as soon as the canvas and
  // viewBox have different aspect ratios.
  const Transform2d canvasFromDoc = editor.document().document().canvasFromDocumentTransform();
  drawChromeWithTransform(renderer, editor.selectedElements(), canvasFromDoc);
}

void OverlayRenderer::drawChrome(svg::RendererInterface& renderer,
                                 const std::optional<svg::SVGElement>& selection) {
  drawChromeWithTransform(renderer, selection, Transform2d());
}

void OverlayRenderer::drawChromeWithTransform(svg::RendererInterface& renderer,
                                              const std::optional<svg::SVGElement>& selection,
                                              const Transform2d& canvasFromDoc) {
  if (!selection.has_value()) {
    return;
  }
  std::array<svg::SVGElement, 1> single{*selection};
  drawChromeWithTransform(renderer, std::span<const svg::SVGElement>(single), canvasFromDoc);
}

void OverlayRenderer::drawChromeWithTransform(svg::RendererInterface& renderer,
                                              std::span<const svg::SVGElement> selection,
                                              const Transform2d& canvasFromDoc) {
  drawChromeWithTransform(renderer, selection, /*marqueeRectDoc=*/std::nullopt, canvasFromDoc);
}

SelectionChromeSnapshot OverlayRenderer::captureChromeSnapshot(
    std::span<const svg::SVGElement> selection, const std::optional<Box2d>& marqueeRectDoc,
    const Transform2d& canvasFromDoc,
    const std::optional<SelectionChromeBoundsPreview>& activeBoundsPreview,
    std::span<const svg::SVGElement> sourceHover, const std::optional<Box2d>& cullRectDoc,
    SelectionChromeDetail selectionDetail, const Transform2d& representedDocumentFromLiveDocument,
    const std::optional<LockedRejectionFlashInput>& lockedFlash, double devicePixelRatio,
    const std::optional<svg::SVGElement>& livePathPreviewElement) {
  SelectionChromeSnapshot snapshot;
  snapshot.canvasFromDoc = canvasFromDoc;
  snapshot.marqueeDoc = marqueeRectDoc;

  if (livePathPreviewElement.has_value()) {
    snapshot.livePathPreview =
        CaptureLivePathPreview(*livePathPreviewElement, representedDocumentFromLiveDocument);
  }

  // Locked-rejection flash: capture the rejected element's document-space outline (merged across
  // its renderable geometry leaves, same path-build path as selection chrome) so the draw phase can
  // stroke it red without touching the registry. Captured unconditionally of selection state - a
  // locked click never selects, so the flashed element is typically NOT in `selection`.
  if (lockedFlash.has_value() && lockedFlash->intensity > 0.0f) {
    std::vector<SelectionChromeSnapshot::PathItem> flashPaths;
    std::vector<Box2d> flashAabbs;
    std::array<svg::SVGElement, 1> flashElements{lockedFlash->element};
    AppendChromeItems(std::span<const svg::SVGElement>(flashElements), /*cullRectDoc=*/std::nullopt,
                      &flashPaths, &flashAabbs, /*outPathAnchorPoints=*/nullptr,
                      /*outPathControlLines=*/nullptr, /*outPathControlPoints=*/nullptr,
                      /*outTextBaselines=*/nullptr,
                      AppendChromeItemsOptions{.includePathPointChrome = false});
    if (!flashPaths.empty()) {
      PathBuilder mergedFlashPath;
      for (const SelectionChromeSnapshot::PathItem& item : flashPaths) {
        mergedFlashPath.addPath(item.pathDoc);
      }
      snapshot.lockedFlash = SelectionChromeSnapshot::LockedFlash{
          .pathDoc = mergedFlashPath.build(),
          .intensity = lockedFlash->intensity,
      };
    }
  }

  // Sizes are NOT baked here: the draw phase resolves them from whatever
  // transform it draws with, so the snapshot stays valid at any zoom.
  snapshot.devicePixelRatio = devicePixelRatio;
  const double scale = LinearScale(canvasFromDoc);

  if (!sourceHover.empty()) {
    AppendChromeItems(sourceHover, cullRectDoc, &snapshot.hoverPaths, &snapshot.hoverAabbsDoc,
                      /*outPathAnchorPoints=*/nullptr, /*outPathControlLines=*/nullptr,
                      /*outPathControlPoints=*/nullptr, /*outTextBaselines=*/nullptr,
                      AppendChromeItemsOptions{.includePathPointChrome = false});
  }

  if (selection.empty()) {
    return snapshot;
  }

  if (selectionDetail == SelectionChromeDetail::EditingChromeOnly) {
    AppendChromeItems(
        selection, cullRectDoc, /*outPaths=*/nullptr, /*outAabbs=*/nullptr,
        /*outPathAnchorPoints=*/nullptr, /*outPathControlLines=*/nullptr,
        /*outPathControlPoints=*/nullptr, &snapshot.textBaselinesDoc,
        AppendChromeItemsOptions{
            .includePaths = false,
            .includePerElementAabbs = false,
            .includePathPointChrome = false,
            .canvasScale = scale,
            .devicePixelRatio = devicePixelRatio,
            .representedDocumentFromLiveDocument = representedDocumentFromLiveDocument,
        });
    return snapshot;
  }

  // Per-element path data + transforms. `computedSpline` and
  // `elementFromWorld` both read registry state - done here, before
  // returning, so the post-return snapshot is fully self-contained.
  const bool combinedBoundsOnly = selectionDetail == SelectionChromeDetail::CombinedBoundsOnly;
  const bool pathOutlinesOnly = selectionDetail == SelectionChromeDetail::PathOutlinesOnly;
  const bool includePathPointChrome = pathOutlinesOnly;

  // A live select gesture carries immutable start bounds and the exact current
  // document transform. Keep its path outline sampled from the live DOM so it
  // scales and rotates with the presented object, while retaining the
  // lightweight oriented-bounds path for handles and omitting per-element
  // AABBs and path-point chrome.
  if (combinedBoundsOnly && activeBoundsPreview.has_value()) {
    AppendChromeItems(
        selection, cullRectDoc, &snapshot.paths, &snapshot.aabbsDoc,
        /*outPathAnchorPoints=*/nullptr, /*outPathControlLines=*/nullptr,
        /*outPathControlPoints=*/nullptr, &snapshot.textBaselinesDoc,
        AppendChromeItemsOptions{
            .includePaths = true,
            .includePerElementAabbs = false,
            .includePathPointChrome = false,
            .canvasScale = scale,
            .devicePixelRatio = devicePixelRatio,
            .representedDocumentFromLiveDocument = representedDocumentFromLiveDocument,
        });
    const auto corners = TransformedBoxCorners(activeBoundsPreview->startBoundsDoc,
                                               activeBoundsPreview->documentFromStartDocument);
    std::array<Vector2d, 4> representedCorners;
    for (std::size_t i = 0; i < corners.size(); ++i) {
      representedCorners[i] = representedDocumentFromLiveDocument.transformPosition(corners[i]);
    }
    snapshot.orientedBoundsDoc = CullOrientedBox(
        SelectionChromeSnapshot::OrientedBox{.cornersDoc = representedCorners}, cullRectDoc);
    if (snapshot.orientedBoundsDoc.has_value()) {
      snapshot.handleAnchorsDoc.assign(representedCorners.begin(), representedCorners.end());
      CullHandleAnchorsInPlace(&snapshot.handleAnchorsDoc, scale, cullRectDoc);
    }
    return snapshot;
  }

  const std::optional<Box2d> combinedSelectionBounds = AppendChromeItems(
      selection, cullRectDoc, &snapshot.paths, &snapshot.aabbsDoc, &snapshot.pathAnchorPointsDoc,
      &snapshot.pathControlLinesDoc, &snapshot.pathControlPointsDoc, &snapshot.textBaselinesDoc,
      AppendChromeItemsOptions{
          .includePaths = !combinedBoundsOnly,
          .includePerElementAabbs = !combinedBoundsOnly && !pathOutlinesOnly,
          .includePathPointChrome = includePathPointChrome,
          .canvasScale = scale,
          .devicePixelRatio = devicePixelRatio,
          .representedDocumentFromLiveDocument = representedDocumentFromLiveDocument,
      });
  if (combinedBoundsOnly && combinedSelectionBounds.has_value()) {
    snapshot.aabbsDoc.push_back(*combinedSelectionBounds);
  }

  // AABBs are computed inline from the selection's current DOM transforms
  // so they track the same frame as the per-element path outlines above.
  // Historically these came from a `SelectionBoundsCache` promoted by the
  // main loop - but that cache lagged the live DOM by 1-2 frames during a
  // drag, producing a visible shear between the path outline (live) and
  // the AABB (cached). Path outline + AABB are now sampled from the same
  // DOM snapshot. The cache is still useful for main-loop selection-
  // changed detection; it's just no longer gating the overlay's bounds.
  if (pathOutlinesOnly) {
    return snapshot;
  }

  if (activeBoundsPreview.has_value() && !snapshot.aabbsDoc.empty()) {
    const auto corners = TransformedBoxCorners(activeBoundsPreview->startBoundsDoc,
                                               activeBoundsPreview->documentFromStartDocument);
    std::array<Vector2d, 4> representedCorners;
    for (std::size_t i = 0; i < corners.size(); ++i) {
      representedCorners[i] = representedDocumentFromLiveDocument.transformPosition(corners[i]);
    }
    snapshot.orientedBoundsDoc = CullOrientedBox(
        SelectionChromeSnapshot::OrientedBox{.cornersDoc = representedCorners}, cullRectDoc);
    if (!snapshot.orientedBoundsDoc.has_value()) {
      snapshot.aabbsDoc.clear();
    }
    snapshot.handleAnchorsDoc.assign(representedCorners.begin(), representedCorners.end());
  } else if (!snapshot.aabbsDoc.empty()) {
    const Box2d combinedBounds = CombinedSelectionBounds(snapshot.aabbsDoc);
    snapshot.handleAnchorsDoc = SelectionTransformCornerPoints(combinedBounds);
  }
  CullHandleAnchorsInPlace(&snapshot.handleAnchorsDoc, scale, cullRectDoc);
  return snapshot;
}

Box2d OverlayRenderer::ChromeSquareForPoint(const SelectionChromeSnapshot& snapshot,
                                            ChromeSquare kind, const Vector2d& pointDoc) {
  const ChromeDrawScale drawScale =
      ChromeDrawScaleFor(snapshot.canvasFromDoc, snapshot.devicePixelRatio);
  switch (kind) {
    case ChromeSquare::PathAnchor:
      return PointBoxForDevicePixels(pointDoc, drawScale.pathAnchorSizeDevicePixels,
                                     drawScale.canvasScale);
    case ChromeSquare::PathControlPoint:
      return PointBoxForDevicePixels(pointDoc, drawScale.pathControlPointSizeDevicePixels,
                                     drawScale.canvasScale);
    case ChromeSquare::TransformHandle: break;
  }
  return HandleBoxForCorner(pointDoc, drawScale.canvasScale);
}

void OverlayRenderer::drawChromeFromSnapshot(svg::RendererInterface& renderer,
                                             const SelectionChromeSnapshot& snapshot) {
  ZoneScopedN("OverlayRenderer::drawChromeFromSnapshot");
  if (snapshot.paths.empty() && snapshot.hoverPaths.empty() && snapshot.aabbsDoc.empty() &&
      snapshot.hoverAabbsDoc.empty() && !snapshot.orientedBoundsDoc.has_value() &&
      snapshot.handleAnchorsDoc.empty() && snapshot.pathAnchorPointsDoc.empty() &&
      snapshot.pathControlLinesDoc.empty() && snapshot.pathControlPointsDoc.empty() &&
      !snapshot.marqueeDoc.has_value() && !snapshot.lockedFlash.has_value() &&
      !snapshot.livePathPreview.has_value() && !snapshot.penPreviewSegmentDoc.has_value() &&
      !snapshot.penCloseAffordanceDoc.has_value() && !snapshot.textCaretDoc.has_value() &&
      snapshot.textSelectionQuadsDoc.empty() && !snapshot.textFrameCornersDoc.has_value() &&
      !snapshot.textBoxDragPreviewDoc.has_value() && snapshot.textBaselinesDoc.empty()) {
    return;
  }

  // Every chrome size below is resolved against the transform this draw uses,
  // never the one capture sampled with. That is what keeps handles at a
  // constant screen size when the same snapshot is redrawn at a new zoom.
  const ChromeDrawScale drawScale =
      ChromeDrawScaleFor(snapshot.canvasFromDoc, snapshot.devicePixelRatio);

  // Live pen-path geometry first: it stands in for the (suppressed) document
  // raster of the edited path, so every chrome layer must draw on top of it.
  if (snapshot.livePathPreview.has_value()) {
    const SelectionChromeSnapshot::LivePathPreview& preview = *snapshot.livePathPreview;
    svg::PaintParams paint;
    paint.opacity = preview.opacity;
    if (preview.fillColor.has_value()) {
      paint.fill = svg::PaintServer::Solid(css::Color(*preview.fillColor));
    } else {
      paint.fill = svg::PaintServer::None{};
    }
    if (preview.strokeColor.has_value()) {
      paint.stroke = svg::PaintServer::Solid(css::Color(*preview.strokeColor));
    } else {
      paint.stroke = svg::PaintServer::None{};
    }
    paint.fillOpacity = preview.fillOpacity;
    paint.strokeOpacity = preview.strokeOpacity;
    paint.strokeParams.strokeWidth = preview.strokeWidthDoc;
    renderer.setPaint(paint);
    renderer.setTransform(snapshot.canvasFromDoc);
    svg::PathShape shape;
    shape.path = preview.pathDoc;
    shape.fillRule = preview.fillRule;
    shape.parentFromEntity = Transform2d();
    renderer.drawPath(shape, paint.strokeParams);
  }

  if (!snapshot.textSelectionQuadsDoc.empty()) {
    const svg::PaintParams selectionFill = MakeTextSelectionFillPaint();
    renderer.setPaint(selectionFill);
    renderer.setTransform(snapshot.canvasFromDoc);
    for (const std::array<Vector2d, 4>& corners : snapshot.textSelectionQuadsDoc) {
      svg::PathShape shape;
      shape.path = PathForCorners(corners);
      shape.parentFromEntity = Transform2d();
      renderer.drawPath(shape, selectionFill.strokeParams);
    }
  }

  // Baseline underlay for selected text: drawn before every other chrome
  // layer so the selection rectangle, handles, and caret all read on top of
  // it. Guidance styling (translucent control-line stroke), not committed-
  // geometry styling.
  if (!snapshot.textBaselinesDoc.empty()) {
    const svg::PaintParams baselinePaint =
        MakePathControlLinePaint(drawScale.selectionStrokeWidthWorld);
    renderer.setPaint(baselinePaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    for (const SelectionChromeSnapshot::TextBaseline& baseline : snapshot.textBaselinesDoc) {
      PathBuilder builder;
      builder.moveTo(baseline.startDoc);
      builder.lineTo(baseline.endDoc);
      svg::PathShape shape;
      shape.path = builder.build();
      shape.parentFromEntity = Transform2d();
      renderer.drawPath(shape, baselinePaint.strokeParams);
    }
  }

  if (!snapshot.hoverPaths.empty()) {
    const svg::PaintParams hoverShapePaint =
        MakeSourceHoverShapePaint(drawScale.hoverStrokeWidthWorld);
    renderer.setPaint(hoverShapePaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    for (const auto& item : snapshot.hoverPaths) {
      svg::PathShape shape;
      shape.path = item.pathDoc;
      shape.parentFromEntity = Transform2d();
      renderer.drawPath(shape, hoverShapePaint.strokeParams);
    }
  } else if (!snapshot.hoverAabbsDoc.empty()) {
    const svg::PaintParams hoverBoundsPaint =
        MakeSourceHoverBoundsPaint(drawScale.hoverStrokeWidthWorld);
    renderer.setPaint(hoverBoundsPaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    for (const Box2d& aabb : snapshot.hoverAabbsDoc) {
      renderer.drawRect(aabb, hoverBoundsPaint.strokeParams);
    }
  }

  const svg::PaintParams selectionStrokePaint =
      MakeSelectionStrokePaint(drawScale.selectionStrokeWidthWorld);
  const svg::PaintParams displayNoneSelectionStrokePaint =
      MakeDisplayNoneSelectionStrokePaint(drawScale.selectionStrokeWidthWorld);

  // Per-element path outlines first - the user sees the exact shape of
  // every selected element regardless of how many are picked.
  if (!snapshot.paths.empty()) {
    renderer.setTransform(snapshot.canvasFromDoc);
    for (const auto& item : snapshot.paths) {
      const svg::PaintParams& paint =
          item.displayNone ? displayNoneSelectionStrokePaint : selectionStrokePaint;
      renderer.setPaint(paint);
      svg::PathShape shape;
      shape.path = item.pathDoc;
      shape.parentFromEntity = Transform2d();
      renderer.drawPath(shape, paint.strokeParams);
    }
  }

  // Pen hover chrome: the rubber-band segment preview strokes with the
  // control-line style (guidance, not committed geometry); the close-path
  // affordance highlights the first anchor with an enlarged handle ring.
  if (snapshot.penPreviewSegmentDoc.has_value()) {
    const svg::PaintParams previewPaint =
        MakePathControlLinePaint(drawScale.selectionStrokeWidthWorld);
    renderer.setPaint(previewPaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    svg::PathShape shape;
    shape.path = *snapshot.penPreviewSegmentDoc;
    shape.parentFromEntity = Transform2d();
    renderer.drawPath(shape, previewPaint.strokeParams);
  }
  if (snapshot.penCloseAffordanceDoc.has_value()) {
    const svg::PaintParams affordancePaint = MakeHandlePaint(drawScale.selectionStrokeWidthWorld);
    renderer.setPaint(affordancePaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    const double halfSizeDoc = drawScale.selectionStrokeWidthWorld * 4.0;
    const Vector2d halfSize(halfSizeDoc, halfSizeDoc);
    renderer.drawRect(Box2d(*snapshot.penCloseAffordanceDoc - halfSize,
                            *snapshot.penCloseAffordanceDoc + halfSize),
                      affordancePaint.strokeParams);
  }

  // Drag-to-create text-box preview: a crisp cyan frame (no fill - the
  // marquee is the one with the translucent fill + white outline), a
  // guidance first-baseline segment, and an I-beam marker (bar + serifs) at
  // the future caret position, so the gesture reads as creating a text box.
  if (snapshot.textBoxDragPreviewDoc.has_value()) {
    const SelectionChromeSnapshot::TextBoxDragPreview& preview = *snapshot.textBoxDragPreviewDoc;
    renderer.setTransform(snapshot.canvasFromDoc);

    const svg::PaintParams framePaint =
        MakeSelectionStrokePaint(drawScale.selectionStrokeWidthWorld);
    renderer.setPaint(framePaint);
    renderer.drawRect(preview.boxDoc, framePaint.strokeParams);

    const svg::PaintParams baselinePaint =
        MakePathControlLinePaint(drawScale.selectionStrokeWidthWorld);
    renderer.setPaint(baselinePaint);
    {
      PathBuilder builder;
      builder.moveTo(preview.baselineStartDoc);
      builder.lineTo(preview.baselineEndDoc);
      svg::PathShape shape;
      shape.path = builder.build();
      shape.parentFromEntity = Transform2d();
      renderer.drawPath(shape, baselinePaint.strokeParams);
    }

    const svg::PaintParams ibeamPaint =
        MakeSelectionStrokePaint(drawScale.selectionStrokeWidthWorld * 1.5);
    renderer.setPaint(ibeamPaint);
    {
      // Serif half-length scales with the bar height so the I-beam keeps its
      // proportions across zoom levels.
      const double serifHalf = std::abs(preview.ibeamBottomDoc.y - preview.ibeamTopDoc.y) * 0.15;
      PathBuilder builder;
      builder.moveTo(preview.ibeamTopDoc);
      builder.lineTo(preview.ibeamBottomDoc);
      builder.moveTo(preview.ibeamTopDoc - Vector2d(serifHalf, 0.0));
      builder.lineTo(preview.ibeamTopDoc + Vector2d(serifHalf, 0.0));
      builder.moveTo(preview.ibeamBottomDoc - Vector2d(serifHalf, 0.0));
      builder.lineTo(preview.ibeamBottomDoc + Vector2d(serifHalf, 0.0));
      svg::PathShape shape;
      shape.path = builder.build();
      shape.parentFromEntity = Transform2d();
      renderer.drawPath(shape, ibeamPaint.strokeParams);
    }
  }

  // Text-editing chrome: the session frame is an oriented quad stroked in
  // the selection style with resize/rotate handle squares at its corners -
  // after a rotate it stays aligned to the text's rotation instead of
  // snapping back to the axis-aligned envelope. The caret is a solid
  // vertical bar in the selection stroke style.
  if (snapshot.textFrameCornersDoc.has_value()) {
    renderer.setTransform(snapshot.canvasFromDoc);
    const double frameOpacity =
        std::clamp(static_cast<double>(snapshot.textFrameOpacity), 0.0, 1.0);
    const svg::PaintParams framePaint =
        MakeSelectionStrokePaint(drawScale.selectionStrokeWidthWorld, frameOpacity);
    renderer.setPaint(framePaint);
    svg::PathShape frameShape;
    frameShape.path = PathForCorners(*snapshot.textFrameCornersDoc);
    frameShape.parentFromEntity = Transform2d();
    renderer.drawPath(frameShape, framePaint.strokeParams);

    // Use the select tool's handle-box helper directly. Deriving this size
    // from stroke width double-counted device scale on high-density displays.
    const svg::PaintParams handlePaint =
        MakeHandlePaint(drawScale.selectionStrokeWidthWorld, frameOpacity);
    renderer.setPaint(handlePaint);
    for (const Vector2d& corner : *snapshot.textFrameCornersDoc) {
      renderer.drawRect(HandleBoxForCorner(corner, drawScale.canvasScale),
                        handlePaint.strokeParams);
    }
  }
  if (snapshot.textCaretDoc.has_value()) {
    const svg::PaintParams caretPaint =
        MakeSelectionStrokePaint(drawScale.selectionStrokeWidthWorld * 1.5);
    renderer.setPaint(caretPaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    PathBuilder caretBuilder;
    caretBuilder.moveTo(snapshot.textCaretDoc->topDoc);
    caretBuilder.lineTo(snapshot.textCaretDoc->bottomDoc);
    svg::PathShape caretShape;
    caretShape.path = caretBuilder.build();
    caretShape.parentFromEntity = Transform2d();
    renderer.drawPath(caretShape, caretPaint.strokeParams);
  }

  if (!snapshot.pathControlLinesDoc.empty()) {
    const svg::PaintParams pathControlLinePaint =
        MakePathControlLinePaint(drawScale.selectionStrokeWidthWorld);
    renderer.setPaint(pathControlLinePaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    for (const SelectionChromeSnapshot::PathControlLine& line : snapshot.pathControlLinesDoc) {
      PathBuilder builder;
      builder.moveTo(line.anchorDoc);
      builder.lineTo(line.controlDoc);
      svg::PathShape shape;
      shape.path = builder.build();
      shape.parentFromEntity = Transform2d();
      renderer.drawPath(shape, pathControlLinePaint.strokeParams);
    }
  }

  if (!snapshot.pathControlPointsDoc.empty()) {
    renderer.setTransform(snapshot.canvasFromDoc);
    const svg::PaintParams controlPointPaint = MakePathPointPaint();
    renderer.setPaint(controlPointPaint);
    for (const Vector2d& controlPointDoc : snapshot.pathControlPointsDoc) {
      renderer.drawRect(
          PointBoxForDevicePixels(controlPointDoc, drawScale.pathControlPointSizeDevicePixels,
                                  drawScale.canvasScale),
          controlPointPaint.strokeParams);
    }
  }

  if (!snapshot.pathAnchorPointsDoc.empty()) {
    renderer.setTransform(snapshot.canvasFromDoc);
    const svg::PaintParams pathAnchorPaint = MakePathPointPaint();
    renderer.setPaint(pathAnchorPaint);
    for (const Vector2d& anchorDoc : snapshot.pathAnchorPointsDoc) {
      renderer.drawRect(PointBoxForDevicePixels(anchorDoc, drawScale.pathAnchorSizeDevicePixels,
                                                drawScale.canvasScale),
                        pathAnchorPaint.strokeParams);
    }
  }

  // Selection AABBs: one rectangle per element, plus a single combined
  // envelope when there are multiple elements. Drawn in document space
  // with `canvasFromDoc` applied so they line up with the content
  // bitmap the compositor produced for the same frame.
  if (snapshot.orientedBoundsDoc.has_value()) {
    renderer.setPaint(selectionStrokePaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    svg::PathShape shape;
    shape.path = PathForCorners(snapshot.orientedBoundsDoc->cornersDoc);
    shape.parentFromEntity = Transform2d();
    renderer.drawPath(shape, selectionStrokePaint.strokeParams);
  } else if (!snapshot.aabbsDoc.empty()) {
    renderer.setPaint(selectionStrokePaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    const Box2d combinedBounds = CombinedSelectionBounds(snapshot.aabbsDoc);
    for (const Box2d& aabb : snapshot.aabbsDoc) {
      renderer.drawRect(aabb, selectionStrokePaint.strokeParams);
    }
    if (snapshot.aabbsDoc.size() > 1) {
      renderer.drawRect(combinedBounds, selectionStrokePaint.strokeParams);
    }
  }

  if (!snapshot.handleAnchorsDoc.empty()) {
    renderer.setTransform(snapshot.canvasFromDoc);
    const svg::PaintParams handlePaint = MakeHandlePaint(drawScale.selectionStrokeWidthWorld);
    renderer.setPaint(handlePaint);
    for (const Vector2d& anchorDoc : snapshot.handleAnchorsDoc) {
      renderer.drawRect(HandleBoxForCorner(anchorDoc, drawScale.canvasScale),
                        handlePaint.strokeParams);
    }
  }

  // Marquee: translucent cyan fill + solid white outline. Two passes
  // (fill then stroke) - different fill/stroke paints per the legacy
  // ImGui styling.
  if (snapshot.marqueeDoc.has_value()) {
    renderer.setTransform(snapshot.canvasFromDoc);
    const svg::PaintParams marqueeFill = MakeMarqueeFillPaint();
    renderer.setPaint(marqueeFill);
    renderer.drawRect(*snapshot.marqueeDoc, marqueeFill.strokeParams);

    const svg::PaintParams marqueeStroke =
        MakeMarqueeStrokePaint(drawScale.marqueeStrokeWidthWorld);
    renderer.setPaint(marqueeStroke);
    renderer.drawRect(*snapshot.marqueeDoc, marqueeStroke.strokeParams);
  }

  // Locked-rejection flash: a red outline of the rejected (locked) element's path, drawn last so it
  // reads on top of all other chrome. The stroke alpha fades with `intensity`. Stroke width tracks
  // the snapshot's pixel-to-world scale just like the selection outline.
  if (snapshot.lockedFlash.has_value() && snapshot.lockedFlash->intensity > 0.0f) {
    const double lockedFlashStrokeWidthWorld =
        drawScale.selectionStrokeWidthWorld > 0.0
            ? drawScale.selectionStrokeWidthWorld *
                  (kLockedFlashStrokeLogicalPixels / kSelectionStrokeLogicalPixels)
            : kLockedFlashStrokeLogicalPixels;
    const svg::PaintParams lockedFlashPaint =
        MakeLockedFlashStrokePaint(lockedFlashStrokeWidthWorld, snapshot.lockedFlash->intensity);
    renderer.setPaint(lockedFlashPaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    svg::PathShape shape;
    shape.path = snapshot.lockedFlash->pathDoc;
    shape.parentFromEntity = Transform2d();
    renderer.drawPath(shape, lockedFlashPaint.strokeParams);
  }
}

void OverlayRenderer::drawChromeWithTransform(
    svg::RendererInterface& renderer, std::span<const svg::SVGElement> selection,
    const std::optional<Box2d>& marqueeRectDoc, const Transform2d& canvasFromDoc,
    const std::optional<SelectionChromeBoundsPreview>& activeBoundsPreview,
    std::span<const svg::SVGElement> sourceHover, const std::optional<Box2d>& cullRectDoc,
    SelectionChromeDetail selectionDetail, const Transform2d& representedDocumentFromLiveDocument) {
  ZoneScopedN("OverlayRenderer::drawChrome");
  // Route the live path through capture + draw so the snapshot
  // implementation is the single source of truth. Same output, same
  // performance characteristics (the capture is straight-line registry
  // reads + a small allocation).
  const SelectionChromeSnapshot snapshot = captureChromeSnapshot(
      selection, marqueeRectDoc, canvasFromDoc, activeBoundsPreview, sourceHover, cullRectDoc,
      selectionDetail, representedDocumentFromLiveDocument);
  drawChromeFromSnapshot(renderer, snapshot);
}

}  // namespace donner::editor
