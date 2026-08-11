#include "donner/editor/SelectionAabb.h"

#include <array>
#include <cmath>
#include <optional>

#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/core/VectorEffect.h"

namespace donner::editor {

namespace {

/// Subtree roots whose descendants are never part of the rendered tree
/// (resource-only containers, CSS stylesheets, etc.). Skipping them
/// prevents chrome from decorating hidden shapes that belong to
/// paint-servers or clip/mask definitions.
bool IsNonRenderedContainer(svg::ElementType type) {
  switch (type) {
    case svg::ElementType::Defs:
    case svg::ElementType::ClipPath:
    case svg::ElementType::Mask:
    case svg::ElementType::Filter:
    case svg::ElementType::Pattern:
    case svg::ElementType::LinearGradient:
    case svg::ElementType::RadialGradient:
    case svg::ElementType::Symbol:
    case svg::ElementType::Marker:
    case svg::ElementType::Style: return true;
    default: return false;
  }
}

bool HasLiveSvgTreeComponents(const svg::SVGElement& element) {
  return xml::XMLNode::TryCast(element.entityHandle()).has_value() && element.tryType().has_value();
}

LineCap ToLineCap(svg::StrokeLinecap cap) {
  switch (cap) {
    case svg::StrokeLinecap::Butt: return LineCap::Butt;
    case svg::StrokeLinecap::Round: return LineCap::Round;
    case svg::StrokeLinecap::Square: return LineCap::Square;
  }
  return LineCap::Butt;
}

LineJoin ToLineJoin(svg::StrokeLinejoin join) {
  switch (join) {
    case svg::StrokeLinejoin::Miter:
    case svg::StrokeLinejoin::MiterClip:
    case svg::StrokeLinejoin::Arcs: return LineJoin::Miter;
    case svg::StrokeLinejoin::Round: return LineJoin::Round;
    case svg::StrokeLinejoin::Bevel: return LineJoin::Bevel;
  }
  return LineJoin::Miter;
}

double EffectiveLocalStrokeWidth(const svg::PropertyRegistry& style,
                                 const Transform2d& documentFromElement) {
  double strokeWidth = style.strokeWidth.get().value().value;
  if (style.vectorEffect.getOr(svg::VectorEffect::None) == svg::VectorEffect::NonScalingStroke) {
    const double scale = std::sqrt(std::abs(documentFromElement.determinant()));
    if (std::isfinite(scale) && scale > 0.0) {
      strokeWidth /= scale;
    }
  }
  return strokeWidth;
}

std::optional<svg::SVGElement> SafeFirstChild(const svg::SVGElement& element) {
  if (!HasLiveSvgTreeComponents(element)) {
    return std::nullopt;
  }

  return element.firstChild();
}

std::optional<svg::SVGElement> SafeNextSibling(const svg::SVGElement& element) {
  if (!xml::XMLNode::TryCast(element.entityHandle()).has_value()) {
    return std::nullopt;
  }

  return element.nextSibling();
}

void CollectRenderableGeometryImpl(const svg::SVGElement& root,
                                   std::vector<svg::SVGGeometryElement>& out) {
  if (!HasLiveSvgTreeComponents(root)) {
    return;
  }

  if (IsNonRenderedContainer(root.type())) {
    return;
  }
  if (root.isa<svg::SVGGeometryElement>()) {
    out.push_back(root.cast<svg::SVGGeometryElement>());
    // Geometry elements have no graphical children worth descending into
    // for outline purposes - stop here.
    return;
  }
  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    CollectRenderableGeometryImpl(current, out);
  }
}

void CollectRenderableTextRootsImpl(const svg::SVGElement& root,
                                    std::vector<svg::SVGTextElement>& out) {
  if (!HasLiveSvgTreeComponents(root)) {
    return;
  }

  if (IsNonRenderedContainer(root.type())) {
    return;
  }
  if (root.isa<svg::SVGTextElement>()) {
    out.push_back(root.cast<svg::SVGTextElement>());
    // Text content (tspans) contributes chrome through its root - stop here.
    return;
  }
  if (root.isa<svg::SVGGeometryElement>()) {
    return;
  }
  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    CollectRenderableTextRootsImpl(current, out);
  }
}

/// Parse @p name as a plain SVG number, or nullopt when absent/malformed.
std::optional<double> ParseNumericAttribute(const svg::SVGElement& element, std::string_view name) {
  const std::optional<RcString> value = element.getAttribute(name);
  if (!value.has_value()) {
    return std::nullopt;
  }
  const auto result = ::donner::parser::NumberParser::Parse(std::string_view(*value));
  if (result.hasError()) {
    return std::nullopt;
  }
  return result.result().number;
}

/// Document-space AABB covering @p localRect mapped through @p text's
/// element transform, corner by corner.
Box2d TextWorldAabbOfLocalRect(const svg::SVGTextElement& text, const Box2d& localRect) {
  const Transform2d documentFromText = text.elementFromWorld();
  const std::array<Vector2d, 4> corners = {
      localRect.topLeft, Vector2d(localRect.bottomRight.x, localRect.topLeft.y),
      localRect.bottomRight, Vector2d(localRect.topLeft.x, localRect.bottomRight.y)};
  Box2d worldRect = Box2d::CreateEmpty(documentFromText.transformPosition(corners[0]));
  for (std::size_t i = 1; i < corners.size(); ++i) {
    worldRect.addPoint(documentFromText.transformPosition(corners[i]));
  }
  return worldRect;
}

/// Merge the document-space bounds of every renderable leaf (geometry world
/// bounds + text frame bounds) in @p root's subtree into @p merged.
void MergeRenderableWorldBounds(const svg::SVGElement& root, std::optional<Box2d>& merged) {
  std::vector<svg::SVGGeometryElement> geometryElements;
  CollectRenderableGeometryImpl(root, geometryElements);
  for (const auto& geometry : geometryElements) {
    const auto wb = GeometryWorldFrameBounds(geometry);
    if (!wb.has_value()) {
      continue;
    }
    if (merged.has_value()) {
      merged->addBox(*wb);
    } else {
      merged = *wb;
    }
  }

  std::vector<svg::SVGTextElement> textRoots;
  CollectRenderableTextRootsImpl(root, textRoots);
  for (const auto& text : textRoots) {
    const std::optional<Box2d> frameDoc = TextWorldFrameBounds(text);
    if (!frameDoc.has_value()) {
      continue;
    }
    if (merged.has_value()) {
      merged->addBox(*frameDoc);
    } else {
      merged = *frameDoc;
    }
  }
}

void CollectLaterRenderableBoundsImpl(const svg::SVGElement& root, const svg::SVGElement& selected,
                                      bool& afterSelected, std::vector<Box2d>& out) {
  if (!HasLiveSvgTreeComponents(root)) {
    return;
  }

  if (IsNonRenderedContainer(root.type())) {
    return;
  }

  if (root == selected) {
    afterSelected = true;
    return;
  }

  if (root.isa<svg::SVGGeometryElement>()) {
    if (afterSelected) {
      const auto wb = GeometryWorldFrameBounds(root.cast<svg::SVGGeometryElement>());
      if (wb.has_value()) {
        out.push_back(*wb);
      }
    }
    return;
  }

  if (root.isa<svg::SVGTextElement>()) {
    if (afterSelected) {
      const std::optional<Box2d> frameDoc = TextWorldFrameBounds(root.cast<svg::SVGTextElement>());
      if (frameDoc.has_value()) {
        out.push_back(*frameDoc);
      }
    }
    return;
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    CollectLaterRenderableBoundsImpl(current, selected, afterSelected, out);
  }
}

}  // namespace

std::vector<svg::SVGGeometryElement> CollectRenderableGeometry(const svg::SVGElement& root) {
  std::vector<svg::SVGGeometryElement> out;
  root.withReadAccess([&out, &root](svg::DocumentReadAccess&, EntityHandle) {
    CollectRenderableGeometryImpl(root, out);
  });
  return out;
}

std::vector<svg::SVGTextElement> CollectRenderableTextRoots(const svg::SVGElement& root) {
  std::vector<svg::SVGTextElement> out;
  root.withReadAccess([&out, &root](svg::DocumentReadAccess&, EntityHandle) {
    CollectRenderableTextRootsImpl(root, out);
  });
  return out;
}

std::optional<Box2d> GeometryWorldFrameBounds(const svg::SVGGeometryElement& geometry) {
  std::optional<Box2d> result = geometry.worldBounds();
  const svg::PropertyRegistry style = geometry.getComputedStyle();
  if (style.stroke.get().value().is<svg::PaintServer::None>()) {
    return result;
  }

  const Transform2d documentFromGeometry = geometry.elementFromWorld();
  const double strokeWidth = EffectiveLocalStrokeWidth(style, documentFromGeometry);
  if (strokeWidth <= 0.0) {
    return result;
  }

  const std::optional<Path> spline = geometry.computedSpline();
  if (!spline.has_value() || spline->empty()) {
    return result;
  }

  StrokeStyle strokeStyle;
  strokeStyle.width = strokeWidth;
  strokeStyle.cap = ToLineCap(style.strokeLinecap.get().value());
  strokeStyle.join = ToLineJoin(style.strokeLinejoin.get().value());
  strokeStyle.miterLimit = style.strokeMiterlimit.get().value();
  // World-space ink bounds are a document-space quantity, not a rasterization:
  // the outline is only used for its bounding box, so a path-local tolerance is
  // correct and keeps the reported bounds independent of the view zoom.
  const Path strokeOutline = spline->strokeToFill(strokeStyle, Path::kLocalFlattenTolerance);
  if (strokeOutline.empty()) {
    return result;
  }

  const Box2d strokeBounds = strokeOutline.transformedBounds(documentFromGeometry);
  if (result.has_value()) {
    result->addBox(strokeBounds);
  } else {
    result = strokeBounds;
  }
  return result;
}

std::optional<Box2d> TextWorldInkBounds(const svg::SVGTextElement& text) {
  const Box2d inkLocal = text.inkBoundingBox();
  if (inkLocal.isEmpty()) {
    return std::nullopt;
  }

  return TextWorldAabbOfLocalRect(text, inkLocal);
}

std::optional<Box2d> AuthoredTextBoxLocal(const svg::SVGTextElement& text) {
  const std::optional<double> boxWidth = ParseNumericAttribute(text, "data-donner-text-box-width");
  const std::optional<double> boxHeight =
      ParseNumericAttribute(text, "data-donner-text-box-height");
  if (!boxWidth.has_value() || !boxHeight.has_value()) {
    return std::nullopt;
  }

  // Legacy box text derives the frame origin from the first baseline. Newer
  // text stores its frame origin independently so a top-edge resize does not
  // move the text baseline. 16 is the CSS `font-size` initial value.
  const double fontSize = ParseNumericAttribute(text, "font-size").value_or(16.0);
  const Vector2d legacyTopLeft(ParseNumericAttribute(text, "x").value_or(0.0),
                               ParseNumericAttribute(text, "y").value_or(0.0) - fontSize);
  const Vector2d topLeft(
      ParseNumericAttribute(text, "data-donner-text-box-x").value_or(legacyTopLeft.x),
      ParseNumericAttribute(text, "data-donner-text-box-y").value_or(legacyTopLeft.y));
  return Box2d(topLeft, topLeft + Vector2d(*boxWidth, *boxHeight));
}

std::optional<Box2d> TextWorldFrameBounds(const svg::SVGTextElement& text) {
  std::optional<Box2d> frameLocal = AuthoredTextBoxLocal(text);
  if (!frameLocal.has_value()) {
    const Box2d objectLocal = text.objectBoundingBox();
    if (objectLocal.isEmpty()) {
      return std::nullopt;
    }
    frameLocal = objectLocal;
  }

  const svg::PropertyRegistry style = text.getComputedStyle();
  if (!style.stroke.get().value().is<svg::PaintServer::None>()) {
    const double strokeWidth = EffectiveLocalStrokeWidth(style, text.elementFromWorld());
    if (strokeWidth > 0.0) {
      const Box2d inkLocal = text.inkBoundingBox();
      if (!inkLocal.isEmpty()) {
        double padding = strokeWidth * 0.5;
        if (ToLineJoin(style.strokeLinejoin.get().value()) == LineJoin::Miter) {
          padding *= style.strokeMiterlimit.get().value();
        }
        frameLocal->addBox(inkLocal.inflatedBy(padding));
      }
    }
  }

  return TextWorldAabbOfLocalRect(text, *frameLocal);
}

std::vector<Box2d> SnapshotSelectionWorldBounds(std::span<const svg::SVGElement> selection) {
  std::vector<Box2d> bounds;
  bounds.reserve(selection.size());
  for (const auto& element : selection) {
    // Expand each selection entry to its renderable leaves and union their
    // world bounds. For a plain geometry selection this is a single-element
    // collection that reduces to stroke-aware geometry bounds; a `<text>` root
    // reduces to its full frame; for a `<g filter>` it unions every descendant shape and
    // text run so the AABB envelopes the visible group.
    std::optional<Box2d> merged;
    element.withWriteAccess([&element, &merged](svg::DocumentWriteAccess&, EntityHandle) {
      MergeRenderableWorldBounds(element, merged);
    });
    if (merged.has_value()) {
      bounds.push_back(*merged);
    }
  }

  return bounds;
}

std::vector<Box2d> SnapshotSelectionOccludingWorldBounds(
    std::span<const svg::SVGElement> selection) {
  if (selection.size() != 1u) {
    return {};
  }

  svg::SVGElement selected = selection.front();
  return selected.withWriteAccess(
      [&selected](svg::DocumentWriteAccess&, EntityHandle) -> std::vector<Box2d> {
        if (!HasLiveSvgTreeComponents(selected)) {
          return {};
        }

        const svg::SVGElement root = selected.ownerDocument().svgElement();
        std::vector<Box2d> laterBounds;
        bool afterSelected = false;
        CollectLaterRenderableBoundsImpl(root, selected, afterSelected, laterBounds);
        return laterBounds;
      });
}

void PromoteSelectionBoundsIfReady(SelectionBoundsCache& cache, std::uint64_t displayedDocVersion) {
  if (cache.pendingVersion != displayedDocVersion) {
    return;
  }

  cache.displayedBoundsDoc = cache.pendingBoundsDoc;
  cache.displayedOccludingBoundsDoc = cache.pendingOccludingBoundsDoc;
  cache.pendingBoundsDoc.clear();
  cache.pendingOccludingBoundsDoc.clear();
  cache.pendingVersion = 0;
}

void RefreshSelectionBoundsCache(SelectionBoundsCache& cache,
                                 std::span<const svg::SVGElement> selection,
                                 std::uint64_t currentDocVersion,
                                 std::uint64_t displayedDocVersion) {
  cache.lastSelection.assign(selection.begin(), selection.end());
  cache.lastRefreshVersion = currentDocVersion;
  cache.pendingBoundsDoc = SnapshotSelectionWorldBounds(selection);
  cache.pendingOccludingBoundsDoc = SnapshotSelectionOccludingWorldBounds(selection);
  cache.pendingVersion = currentDocVersion;

  if (selection.empty()) {
    cache.displayedBoundsDoc.clear();
    cache.displayedOccludingBoundsDoc.clear();
  }

  PromoteSelectionBoundsIfReady(cache, displayedDocVersion);
}

}  // namespace donner::editor
