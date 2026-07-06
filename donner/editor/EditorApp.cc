#include "donner/editor/EditorApp.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "donner/base/FormatNumber.h"
#include "donner/base/PathOps.h"
#include "donner/css/CSS.h"
#include "donner/css/Declaration.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/LockState.h"
#include "donner/editor/TextPatch.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/properties/PaintServer.h"

namespace donner::editor {

namespace {

/// AABB-vs-AABB intersection test. Returns true if the two boxes
/// overlap by any non-zero amount, including edge-touching contact.
/// Used by `hitTestRect` to decide which elements a marquee covers.
bool BoxesIntersect(const Box2d& a, const Box2d& b) {
  return a.topLeft.x <= b.bottomRight.x && a.bottomRight.x >= b.topLeft.x &&
         a.topLeft.y <= b.bottomRight.y && a.bottomRight.y >= b.topLeft.y;
}

Path RectPath(const Box2d& box) {
  return PathBuilder()
      .moveTo(box.topLeft)
      .lineTo(Vector2d(box.bottomRight.x, box.topLeft.y))
      .lineTo(box.bottomRight)
      .lineTo(Vector2d(box.topLeft.x, box.bottomRight.y))
      .closePath()
      .build();
}

std::array<Vector2d, 4> BoxCorners(const Box2d& box) {
  return {
      box.topLeft,
      Vector2d(box.bottomRight.x, box.topLeft.y),
      box.bottomRight,
      Vector2d(box.topLeft.x, box.bottomRight.y),
  };
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
    case svg::StrokeLinejoin::Miter: return LineJoin::Miter;
    case svg::StrokeLinejoin::MiterClip: return LineJoin::Miter;
    case svg::StrokeLinejoin::Round: return LineJoin::Round;
    case svg::StrokeLinejoin::Bevel: return LineJoin::Bevel;
    case svg::StrokeLinejoin::Arcs: return LineJoin::Miter;
  }
  return LineJoin::Miter;
}

bool PathEndpointIntersectsRect(const Path& path, const Transform2d& documentFromPath,
                                const Box2d& documentRect) {
  bool intersects = false;
  path.forEach([&](Path::Verb /*verb*/, std::span<const Vector2d> points) {
    if (points.empty()) {
      return;
    }

    if (documentRect.contains(documentFromPath.transformPosition(points.back()))) {
      intersects = true;
    }
  });
  return intersects;
}

int CountRectCornersInsidePath(const Path& path, FillRule fillRule,
                               const Transform2d& documentFromPath, const Box2d& documentRect) {
  if (std::abs(documentFromPath.determinant()) < 1e-12) {
    return 0;
  }

  const Transform2d pathFromDocument = documentFromPath.inverse();
  int insideCount = 0;
  for (const Vector2d& corner : BoxCorners(documentRect)) {
    if (path.isInside(pathFromDocument.transformPosition(corner), fillRule)) {
      ++insideCount;
    }
  }
  return insideCount;
}

bool FilledPathIntersectsRect(const Path& path, FillRule fillRule,
                              const Transform2d& documentFromPath, const Box2d& documentRect) {
  if (PathEndpointIntersectsRect(path, documentFromPath, documentRect)) {
    return true;
  }

  const int insideCornerCount =
      CountRectCornersInsidePath(path, fillRule, documentFromPath, documentRect);
  if (insideCornerCount == 4) {
    // The marquee is fully inside this filled shape. Do not select large containing geometry
    // (backgrounds/glows) unless its own boundary or vertices enter the marquee.
    return false;
  }
  if (insideCornerCount > 0) {
    return true;
  }

  const std::array<PathBooleanInput, 2> inputs = {
      PathBooleanInput{
          .path = path,
          .fillRule = fillRule,
          .outputFromPath = documentFromPath,
      },
      PathBooleanInput{
          .path = RectPath(documentRect),
          .fillRule = FillRule::NonZero,
          .outputFromPath = Transform2d(),
      },
  };
  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Intersect, inputs);
  return result.status == PathBooleanStatus::Ok;
}

bool GeometryIntersectsRect(const svg::SVGGeometryElement& geometry, const Box2d& documentRect) {
  std::optional<Box2d> bounds = geometry.worldBounds();
  if (!bounds.has_value()) {
    return false;
  }

  const auto style = geometry.getComputedStyle();
  const double strokeWidth = style.strokeWidth.get().value().value;
  const bool hasStroke =
      strokeWidth > 0.0 && !style.stroke.get().value().is<svg::PaintServer::None>();
  const Box2d interactionBounds =
      hasStroke ? bounds->inflatedBy(strokeWidth * style.strokeMiterlimit.get().value()) : *bounds;
  if (!BoxesIntersect(interactionBounds, documentRect)) {
    return false;
  }

  std::optional<Path> spline = geometry.computedSpline();
  if (!spline.has_value() || spline->empty()) {
    return false;
  }

  const Transform2d documentFromGeometry = geometry.elementFromWorld();
  if (!style.fill.get().value().is<svg::PaintServer::None>() &&
      FilledPathIntersectsRect(*spline, style.fillRule.get().value(), documentFromGeometry,
                               documentRect)) {
    return true;
  }

  if (hasStroke) {
    StrokeStyle strokeStyle;
    strokeStyle.width = strokeWidth;
    strokeStyle.cap = ToLineCap(style.strokeLinecap.get().value());
    strokeStyle.join = ToLineJoin(style.strokeLinejoin.get().value());
    strokeStyle.miterLimit = style.strokeMiterlimit.get().value();
    const Path strokePath = spline->strokeToFill(strokeStyle);
    return FilledPathIntersectsRect(strokePath, FillRule::NonZero, documentFromGeometry,
                                    documentRect);
  }

  return false;
}

/// Depth-first walk of the SVG tree rooted at `node`, invoking
/// `visit(geometry)` on every `SVGGeometryElement` encountered. Used
/// by `hitTestRect` so marquee selection lives entirely on top of
/// the public DOM API - no ECS reach-through.
template <typename Visitor>
void ForEachGeometryElement(const svg::SVGElement& node, Visitor& visit) {
  if (node.isa<svg::SVGGeometryElement>()) {
    visit(node.cast<svg::SVGGeometryElement>());
  }
  for (auto child = node.firstChild(); child.has_value(); child = child->nextSibling()) {
    ForEachGeometryElement(*child, visit);
  }
}

/// Depth-first SVG tree walk in paint order, including non-geometry containers.
template <typename Visitor>
void ForEachElement(const svg::SVGElement& node, Visitor& visit) {
  visit(node);
  for (auto child = node.firstChild(); child.has_value(); child = child->nextSibling()) {
    ForEachElement(*child, visit);
  }
}

/// Whether the laid-out ink bounds of text root @p text intersect
/// @p documentRect. The local ink box is transformed to document space and
/// tested conservatively as an axis-aligned box, matching the pointer
/// contract for selecting text (any point inside the glyph extents hits).
bool TextIntersectsRect(const svg::SVGTextElement& text, const Box2d& documentRect) {
  const Box2d inkLocal = text.inkBoundingBox();
  if (inkLocal.isEmpty()) {
    return false;
  }
  const Transform2d documentFromText = text.elementFromWorld();
  Box2d inkDoc(documentFromText.transformPosition(inkLocal.topLeft),
               documentFromText.transformPosition(inkLocal.topLeft));
  for (const Vector2d& corner : BoxCorners(inkLocal)) {
    inkDoc.addPoint(documentFromText.transformPosition(corner));
  }
  return BoxesIntersect(inkDoc, documentRect);
}

/// Depth-first walk of the SVG tree rooted at `node`, invoking
/// `visit(element)` on every selectable leaf: geometry elements and `<text>`
/// roots (tspans select through their text root). Shared by `hitTestRect`
/// and `selectableElements` so marquee selection and "Select All" agree.
template <typename Visitor>
void ForEachSelectableElement(const svg::SVGElement& node, Visitor& visit) {
  if (node.isa<svg::SVGGeometryElement>() || node.isa<svg::SVGTextElement>()) {
    visit(node.cast<svg::SVGGraphicsElement>());
  }
  for (auto child = node.firstChild(); child.has_value(); child = child->nextSibling()) {
    ForEachSelectableElement(*child, visit);
  }
}

std::optional<std::string> MergeStyleProperty(std::string_view existingStyle,
                                              std::string_view propertyName,
                                              std::string_view propertyValue) {
  const std::string update = std::string(propertyName) + ": " + std::string(propertyValue);
  std::vector<css::Declaration> updateDeclarations = css::CSS::ParseStyleAttribute(update);
  if (updateDeclarations.empty()) {
    return std::nullopt;
  }

  const std::vector<css::Declaration> existingDeclarations =
      css::CSS::ParseStyleAttribute(existingStyle);
  return css::mergeStyleDeclarations(existingDeclarations, updateDeclarations);
}

struct PathOperationSelection {
  std::vector<PathBooleanInput> inputs;
};

struct CompoundPathSplit {
  std::vector<Path> components;
};

std::vector<svg::SVGElement> SortSelectionByPaintOrder(const svg::SVGDocument& document,
                                                       std::span<const svg::SVGElement> selection) {
  std::vector<svg::SVGElement> result;
  result.reserve(selection.size());

  auto visit = [&](const svg::SVGElement& element) {
    if (std::find(selection.begin(), selection.end(), element) != selection.end()) {
      result.push_back(element);
    }
  };
  ForEachElement(document.svgElement(), visit);
  return result;
}

PathOperationSelection CollectPathOperationSelection(std::span<const svg::SVGElement> selection) {
  PathOperationSelection result;
  result.inputs.reserve(selection.size());

  for (const svg::SVGElement& element : selection) {
    if (!element.isa<svg::SVGGeometryElement>()) {
      continue;
    }

    const svg::SVGGeometryElement geometry = element.cast<svg::SVGGeometryElement>();
    std::optional<Path> spline = geometry.computedSpline();
    if (!spline.has_value() || spline->empty()) {
      continue;
    }

    const Transform2d documentFromElement = geometry.elementFromWorld();
    result.inputs.push_back(PathBooleanInput{
        .path = std::move(*spline),
        .fillRule = geometry.getComputedStyle().fillRule.get().value(),
        .outputFromPath = documentFromElement,
    });
  }

  return result;
}

PathBooleanOp BooleanOpForEditorOperation(PathOperationKind operation) {
  switch (operation) {
    case PathOperationKind::Union: return PathBooleanOp::Union;
    case PathOperationKind::Intersect: return PathBooleanOp::Intersect;
    case PathOperationKind::SubtractFront: return PathBooleanOp::Difference;
    case PathOperationKind::SubtractBack: return PathBooleanOp::Difference;
    case PathOperationKind::Exclude: return PathBooleanOp::Xor;
  }
  return PathBooleanOp::Union;
}

PathBooleanOptions EditorPathBooleanOptions() {
  return PathBooleanOptions{
      .geometricTolerance = 1e-3,
      .maxCurveCount = 20000,
      .maxIntersections = 20000,
      .maxOutputCommands = 8192,
  };
}

std::vector<PathBooleanInput> InputsForEditorOperation(PathOperationKind operation,
                                                       const PathOperationSelection& selection) {
  if (operation != PathOperationKind::SubtractBack || selection.inputs.empty()) {
    return selection.inputs;
  }

  std::vector<PathBooleanInput> inputs;
  inputs.reserve(selection.inputs.size());
  inputs.push_back(selection.inputs.back());
  inputs.insert(inputs.end(), selection.inputs.begin(), selection.inputs.end() - 1);
  return inputs;
}

Path CombineBooleanPaths(std::span<const Path> paths) {
  PathBuilder builder;
  for (const Path& path : paths) {
    builder.addPath(path);
  }
  return builder.build();
}

Path TransformPath(const Path& path, const Transform2d& outputFromPath) {
  PathBuilder builder;
  const std::span<const Vector2d> points = path.points();
  for (const Path::Command& command : path.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        builder.moveTo(outputFromPath.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::LineTo:
        builder.lineTo(outputFromPath.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::QuadTo:
        builder.quadTo(outputFromPath.transformPosition(points[command.pointIndex]),
                       outputFromPath.transformPosition(points[command.pointIndex + 1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(outputFromPath.transformPosition(points[command.pointIndex]),
                        outputFromPath.transformPosition(points[command.pointIndex + 1]),
                        outputFromPath.transformPosition(points[command.pointIndex + 2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  }
  return builder.build();
}

svg::SVGElement BaseElementForPathOperation(PathOperationKind operation,
                                            std::span<const svg::SVGElement> selection) {
  switch (operation) {
    case PathOperationKind::SubtractFront: return selection.front();
    case PathOperationKind::Union:
    case PathOperationKind::Intersect:
    case PathOperationKind::SubtractBack:
    case PathOperationKind::Exclude: return selection.back();
  }
  return selection.front();
}

std::optional<Box2d> InputUnionBounds(const PathOperationSelection& selection) {
  std::optional<Box2d> bounds;
  for (const PathBooleanInput& input : selection.inputs) {
    const Box2d inputBounds = input.path.transformedBounds(input.outputFromPath);
    if (bounds.has_value()) {
      bounds->addBox(inputBounds);
    } else {
      bounds = inputBounds;
    }
  }
  return bounds;
}

std::optional<Box2d> InputIntersectionBounds(const PathOperationSelection& selection) {
  std::optional<Box2d> bounds;
  for (const PathBooleanInput& input : selection.inputs) {
    const Box2d inputBounds = input.path.transformedBounds(input.outputFromPath);
    if (!bounds.has_value()) {
      bounds = inputBounds;
      continue;
    }

    bounds->topLeft.x = std::max(bounds->topLeft.x, inputBounds.topLeft.x);
    bounds->topLeft.y = std::max(bounds->topLeft.y, inputBounds.topLeft.y);
    bounds->bottomRight.x = std::min(bounds->bottomRight.x, inputBounds.bottomRight.x);
    bounds->bottomRight.y = std::min(bounds->bottomRight.y, inputBounds.bottomRight.y);
    if (bounds->width() <= 0.0 || bounds->height() <= 0.0) {
      return std::nullopt;
    }
  }
  return bounds;
}

bool BoxContainsBox(const Box2d& outer, const Box2d& inner, double tolerance) {
  return inner.topLeft.x >= outer.topLeft.x - tolerance &&
         inner.topLeft.y >= outer.topLeft.y - tolerance &&
         inner.bottomRight.x <= outer.bottomRight.x + tolerance &&
         inner.bottomRight.y <= outer.bottomRight.y + tolerance;
}

bool IsCopiedUnbundleAttribute(const xml::XMLQualifiedNameRef& name) {
  return name != xml::XMLQualifiedNameRef("d") && name != xml::XMLQualifiedNameRef("id");
}

std::vector<xml::XMLQualifiedName> CopiedAttributeNames(const svg::SVGElement& source) {
  std::vector<xml::XMLQualifiedName> result;
  source.withReadAccess([&source, &result](const svg::DocumentReadAccess&, EntityHandle) {
    const SmallVector<xml::XMLQualifiedNameRef, 10> names = source.attributes();
    result.reserve(names.size());
    for (const xml::XMLQualifiedNameRef& name : names) {
      if (!IsCopiedUnbundleAttribute(name)) {
        continue;
      }
      result.emplace_back(RcString(name.namespacePrefix), RcString(name.name));
    }
  });
  return result;
}

void CopyUnbundleAttributes(const svg::SVGElement& source, svg::SVGPathElement& target) {
  for (const xml::XMLQualifiedName& name : CopiedAttributeNames(source)) {
    const xml::XMLQualifiedNameRef nameRef(name);
    std::optional<RcString> value = source.getAttribute(nameRef);
    if (value.has_value()) {
      target.setAttribute(nameRef, std::string_view(*value));
    }
  }
}

void AppendCommandToBuilder(PathBuilder& builder, Path::Verb verb,
                            std::span<const Vector2d> points) {
  switch (verb) {
    case Path::Verb::MoveTo: builder.moveTo(points[0]); break;
    case Path::Verb::LineTo: builder.lineTo(points[0]); break;
    case Path::Verb::QuadTo: builder.quadTo(points[0], points[1]); break;
    case Path::Verb::CurveTo: builder.curveTo(points[0], points[1], points[2]); break;
    case Path::Verb::ClosePath: builder.closePath(); break;
  }
}

std::vector<Path> ExtractCompoundPathContours(const Path& path) {
  std::vector<Path> contours;
  PathBuilder builder;
  bool activeContour = false;
  bool contourHasSegment = false;

  const auto flushContour = [&]() {
    if (!activeContour || !contourHasSegment) {
      builder = PathBuilder();
      activeContour = false;
      contourHasSegment = false;
      return;
    }

    contours.push_back(builder.build());
    activeContour = false;
    contourHasSegment = false;
  };

  path.forEach([&](Path::Verb verb, std::span<const Vector2d> points) {
    if (verb == Path::Verb::MoveTo) {
      flushContour();
      builder.moveTo(points[0]);
      activeContour = true;
      return;
    }

    if (!activeContour) {
      return;
    }

    AppendCommandToBuilder(builder, verb, points);
    if (verb != Path::Verb::ClosePath) {
      contourHasSegment = true;
    }
  });
  flushContour();

  return contours;
}

CompoundPathSplit SplitCompoundPathIntoContours(const Path& path) {
  return CompoundPathSplit{
      .components = ExtractCompoundPathContours(path),
  };
}

std::optional<svg::SVGElement> ResolveCompoundPathUnbundleTarget(
    const EditorApp& app, std::optional<svg::SVGElement> target) {
  if (target.has_value()) {
    return target;
  }

  if (app.selectedElements().size() == 1u) {
    return app.selectedElements().front();
  }

  return std::nullopt;
}

bool PathOperationResultFitsInputBounds(const Path& result,
                                        const PathOperationSelection& selection) {
  const std::optional<Box2d> inputBounds = InputUnionBounds(selection);
  if (!inputBounds.has_value()) {
    return false;
  }
  constexpr double kResultBoundsTolerance = 0.5;
  return BoxContainsBox(*inputBounds, result.bounds(), kResultBoundsTolerance);
}

void CopyPathOperationStyle(const svg::SVGElement& source, svg::SVGPathElement& target) {
  constexpr std::array<const char*, 8> kCopiedAttributes = {
      "class",        "fill",           "fill-opacity", "stroke",
      "stroke-width", "stroke-opacity", "opacity",      "fill-rule",
  };

  for (const char* name : kCopiedAttributes) {
    if (std::optional<RcString> value = source.getAttribute(name); value.has_value()) {
      target.setAttribute(name, std::string_view(*value));
    }
  }
}

std::vector<AttributeWritebackTarget> CaptureSelectionTargets(
    std::span<const svg::SVGElement> selection) {
  std::vector<AttributeWritebackTarget> targets;
  targets.reserve(selection.size());
  for (const svg::SVGElement& element : selection) {
    if (std::optional<AttributeWritebackTarget> target = captureAttributeWritebackTarget(element);
        target.has_value()) {
      targets.push_back(std::move(*target));
    }
  }
  return targets;
}

svg::SVGElement ResolveSnapshotElement(AsyncSVGDocument& document, const UndoSnapshot& snapshot) {
  svg::SVGElement liveElement = snapshot.element;
  if (document.hasDocument() && snapshot.writebackTarget.has_value()) {
    if (auto resolved =
            resolveAttributeWritebackTarget(document.document(), *snapshot.writebackTarget);
        resolved.has_value()) {
      liveElement = *resolved;
    }
  }
  return liveElement;
}

void ApplyTimelineSnapshot(EditorApp& app, AsyncSVGDocument& document,
                           const UndoSnapshot& snapshot) {
  if (snapshot.kind == UndoSnapshot::Kind::DocumentSource) {
    app.restoreSelectionAfterNextDocumentReplace(snapshot.selectionTargets);
    app.applyMutation(EditorCommand::ReplaceDocumentCommand(snapshot.documentSource,
                                                            /*preserveUndoOnReparse=*/true));
    return;
  }

  svg::SVGElement liveElement = ResolveSnapshotElement(document, snapshot);

  // Route the restored transform through the command queue so every
  // DOM write - tool drags, text-pane re-parse, and undo - goes through
  // the same mutation seam. The queue coalesces with any pending
  // commands and applies on the next `flushFrame()`.
  app.applyMutation(EditorCommand::SetTransformCommand(liveElement, snapshot.transform));

  // Capture the source-text writeback target BEFORE the command drains
  // so the path-based target resolves against the in-sync document.
  // The writeback will be applied by `main.cc` after `flushFrame()`
  // lands the undone transform on the element - at that point the
  // transform the user sees on the canvas and the `transform=` value
  // in the source must agree. Without this the DOM reverts but the
  // source keeps the post-drag text, and the next edit lands on the
  // wrong baseline.
  if (snapshot.writebackTarget.has_value()) {
    app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
        .target = *snapshot.writebackTarget,
        .transform = snapshot.transform,
        .sourceTransformAttributeValue = snapshot.sourceTransformAttributeValue,
        .restoreSourceTransformAttributeValue = snapshot.restoreSourceTransformAttributeValue});
  } else if (auto target = captureAttributeWritebackTarget(liveElement); target.has_value()) {
    app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
        .target = std::move(*target),
        .transform = snapshot.transform,
        .sourceTransformAttributeValue = snapshot.sourceTransformAttributeValue,
        .restoreSourceTransformAttributeValue = snapshot.restoreSourceTransformAttributeValue});
  }

  for (const UndoSnapshot& extra : snapshot.extras) {
    ApplyTimelineSnapshot(app, document, extra);
  }
}

/// Characters allowed in an SVG id (matches the clipboard id scanner).
bool IsIdChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == ':' || c == '.';
}

/// If @p value references @p oldId via `url(#oldId)` or a whole-value `#oldId`
/// (an `href` target), return the value with every such reference repointed to
/// @p newId; otherwise return `std::nullopt` (no reference, no change).
std::optional<std::string> RewriteIdReferenceInValue(std::string_view value, std::string_view oldId,
                                                     std::string_view newId) {
  // Whole-value local reference, e.g. href="#oldId".
  if (value.size() == oldId.size() + 1u && value.front() == '#' && value.substr(1) == oldId) {
    return "#" + std::string(newId);
  }

  // url(#oldId) occurrences anywhere in the value (covers presentation
  // attributes and inline `style="fill:url(#oldId)"`).
  std::string out;
  bool changed = false;
  std::size_t i = 0;
  while (i < value.size()) {
    if (value.compare(i, 5, "url(#") == 0) {
      const std::size_t start = i + 5;
      std::size_t end = start;
      while (end < value.size() && IsIdChar(value[end])) {
        ++end;
      }
      if (value.substr(start, end - start) == oldId) {
        out.append("url(#").append(newId);
        i = end;
        changed = true;
        continue;
      }
    }
    out.push_back(value[i]);
    ++i;
  }
  return changed ? std::optional<std::string>(std::move(out)) : std::nullopt;
}

/// Rewrite `#oldId` CSS id tokens to `#newId` inside a `<style>` element's
/// text content, in the positions where a `#token` can actually reference the
/// element: id selectors (selector preludes, including inside conditional
/// group rules like `@media`) and `url(#id)` references inside declaration
/// values. A `#token` elsewhere in a declaration value is a hex color literal
/// (an id like `abc` is also a valid color), so it is left untouched, as are
/// comments and quoted strings. A match requires the exact token: `#` +
/// @p oldId + a non-id-character boundary, so `#oldIdSuffix` never matches.
/// Returns the rewritten text if anything changed, otherwise `std::nullopt`.
std::optional<std::string> RewriteIdSelectorInStyle(std::string_view value, std::string_view oldId,
                                                    std::string_view newId) {
  std::string out;
  bool changed = false;
  std::size_t i = 0;
  // Stack of open blocks: `true` = the block holds nested rules (an at-rule
  // body such as `@media { ... }`), so `#token`s inside it are back in
  // selector position; `false` = a qualified rule's declaration block.
  std::vector<bool> blockHoldsRules;
  // True when the current block context is selector/prelude position.
  const auto inSelectorPosition = [&]() {
    return blockHoldsRules.empty() || blockHoldsRules.back();
  };
  // Whether the prelude currently being scanned starts with '@' (an at-rule,
  // whose `{` opens a rule-holding block rather than declarations).
  bool preludeIsAtRule = false;
  bool preludeSeenNonSpace = false;

  while (i < value.size()) {
    const char c = value[i];
    // Comments copy through verbatim.
    if (c == '/' && i + 1 < value.size() && value[i + 1] == '*') {
      const std::size_t end = value.find("*/", i + 2);
      const std::size_t stop = end == std::string_view::npos ? value.size() : end + 2;
      out.append(value.substr(i, stop - i));
      i = stop;
      continue;
    }
    // Quoted strings copy through verbatim (backslash escapes respected).
    if (c == '"' || c == '\'') {
      std::size_t end = i + 1;
      while (end < value.size() && value[end] != c) {
        end += (value[end] == '\\' && end + 1 < value.size()) ? 2 : 1;
      }
      const std::size_t stop = std::min(end + 1, value.size());
      out.append(value.substr(i, stop - i));
      i = stop;
      continue;
    }
    if (c == '{') {
      blockHoldsRules.push_back(inSelectorPosition() && preludeIsAtRule);
      preludeIsAtRule = false;
      preludeSeenNonSpace = false;
      out.push_back(c);
      ++i;
      continue;
    }
    if (c == '}') {
      if (!blockHoldsRules.empty()) {
        blockHoldsRules.pop_back();
      }
      preludeIsAtRule = false;
      preludeSeenNonSpace = false;
      out.push_back(c);
      ++i;
      continue;
    }
    if (c == ';') {
      preludeIsAtRule = false;
      preludeSeenNonSpace = false;
      out.push_back(c);
      ++i;
      continue;
    }
    if (!preludeSeenNonSpace && !std::isspace(static_cast<unsigned char>(c))) {
      preludeSeenNonSpace = true;
      preludeIsAtRule = c == '@';
    }
    if (c == '#' && value.compare(i + 1, oldId.size(), oldId) == 0) {
      const std::size_t after = i + 1 + oldId.size();
      const bool tokenBoundary = after >= value.size() || !IsIdChar(value[after]);
      // Inside a declaration block, only a `url(#id)` functional reference
      // repoints; a bare `#token` there is a color literal.
      bool isUrlReference = false;
      if (!inSelectorPosition()) {
        std::size_t back = out.size();
        while (back > 0 && std::isspace(static_cast<unsigned char>(out[back - 1]))) {
          --back;
        }
        isUrlReference = back >= 4 && out.compare(back - 4, 4, "url(") == 0;
      }
      if (tokenBoundary && (inSelectorPosition() || isUrlReference)) {
        out.push_back('#');
        out.append(newId);
        i = after;
        changed = true;
        continue;
      }
    }
    out.push_back(c);
    ++i;
  }
  return changed ? std::optional<std::string>(std::move(out)) : std::nullopt;
}

/// Depth-first list of every element in the document tree rooted at @p root.
void CollectElements(const svg::SVGElement& root, std::vector<svg::SVGElement>& out) {
  out.push_back(root);
  for (std::optional<svg::SVGElement> child = root.firstChild(); child.has_value();
       child = child->nextSibling()) {
    CollectElements(*child, out);
  }
}

}  // namespace

EditorApp::EditorApp() = default;

bool EditorApp::loadFromString(std::string_view svgBytes) {
  selection_.clear();
  refreshFirstSelectionCache();
  controller_.reset();
  undoTimeline_.clear();
  pendingTransformWritebacks_.clear();
  pendingElementRemoveWritebacks_.clear();
  pendingDocumentSourceUndo_.reset();
  pendingSelectionRestoreTargets_.reset();
  const bool result = document_.loadFromString(svgBytes);
  // A successful load resets the dirty state - the in-memory document
  // now matches the last-loaded bytes. `setCurrentFilePath` should be
  // called separately by the caller if the bytes came from a file.
  if (result) {
    isDirty_ = false;
  }
  return result;
}

void EditorApp::restoreSelectionAfterNextDocumentReplace(
    std::vector<AttributeWritebackTarget> targets) {
  pendingSelectionRestoreTargets_ = std::move(targets);
}

bool EditorApp::revertToCleanSource() {
  const std::string source(cleanSourceText_);
  if (source.empty()) {
    return false;
  }

  if (!loadFromString(source)) {
    return false;
  }

  setCleanSourceText(source);
  return true;
}

bool EditorApp::flushFrame() {
  std::optional<svg::SVGDocument> documentBeforeFlush;
  std::vector<AttributeWritebackTarget> selectionTargets;
  if (document_.hasDocument()) {
    documentBeforeFlush = document_.document();
    selectionTargets.reserve(selection_.size());
    for (const auto& element : selection_) {
      if (auto target = captureAttributeWritebackTarget(element); target.has_value()) {
        selectionTargets.push_back(std::move(*target));
      }
    }
  }

  if (!document_.flushFrame()) {
    // A no-op flush still consumes a deferred source-undo entry: a tool that
    // flushes per keystroke (a text session) reaches its commit with nothing
    // queued, and the entry itself compares before/after source.
    consumePendingDocumentSourceUndo();
    return false;
  }

  const auto& documentFlush = document_.lastFlushResult();
  if (documentBeforeFlush.has_value() && document_.hasDocument() &&
      !(*documentBeforeFlush == document_.document())) {
    std::vector<svg::SVGElement> remappedSelection;
    remappedSelection.reserve(selectionTargets.size());
    for (const auto& target : selectionTargets) {
      if (auto element = resolveAttributeWritebackTarget(document_.document(), target);
          element.has_value()) {
        remappedSelection.push_back(*element);
      }
    }

    selection_ = std::move(remappedSelection);
    refreshFirstSelectionCache();
    controller_.reset();
    controllerVersion_ = 0;
    if (!(documentFlush.replacedDocument && documentFlush.preserveUndoOnReparse)) {
      undoTimeline_.clear();
    }
  }

  if (pendingSelectionRestoreTargets_.has_value() && document_.hasDocument()) {
    std::vector<svg::SVGElement> restoredSelection;
    restoredSelection.reserve(pendingSelectionRestoreTargets_->size());
    for (const AttributeWritebackTarget& target : *pendingSelectionRestoreTargets_) {
      if (auto element = resolveAttributeWritebackTarget(document_.document(), target);
          element.has_value()) {
        restoredSelection.push_back(*element);
      }
    }

    selection_ = std::move(restoredSelection);
    refreshFirstSelectionCache();
    controller_.reset();
    controllerVersion_ = 0;
    pendingSelectionRestoreTargets_.reset();
  }

  consumePendingDocumentSourceUndo();

  return true;
}

void EditorApp::consumePendingDocumentSourceUndo() {
  if (!pendingDocumentSourceUndo_.has_value()) {
    return;
  }
  if (document_.hasDocument() && document_.document().hasSourceStore()) {
    std::string sourceAfter(document_.document().source());
    if (sourceAfter != pendingDocumentSourceUndo_->before.documentSource) {
      UndoSnapshot after =
          captureDocumentSourceSnapshot(pendingDocumentSourceUndo_->before.element, sourceAfter);
      after.selectionTargets = CaptureSelectionTargets(selection_);
      undoTimeline_.record(pendingDocumentSourceUndo_->label,
                           std::move(pendingDocumentSourceUndo_->before), std::move(after));
    }
  }
  pendingDocumentSourceUndo_.reset();
}

bool EditorApp::deleteSelectionWithUndo(std::string_view currentSourceText) {
  if (selection_.empty()) {
    return false;
  }

  // Locked elements are protected from deletion. Filter them out before ANY
  // delete side effect - source undo, remove writebacks, selection change.
  // Gating only the DOM command (applyMutation's IsLockGatedCommand check)
  // would still let the already-enqueued remove writeback splice the locked
  // element out of the source text.
  std::vector<svg::SVGElement> selected;
  std::vector<svg::SVGElement> lockedSelection;
  selected.reserve(selection_.size());
  for (const auto& element : selection_) {
    if (IsLocked(element)) {
      lockedSelection.push_back(element);
    } else {
      selected.push_back(element);
    }
  }
  if (selected.empty()) {
    return false;
  }
  std::vector<std::optional<AttributeWritebackTarget>> writebackTargets;
  writebackTargets.reserve(selected.size());

  std::vector<TextPatch> removePatches;
  removePatches.reserve(selected.size());
  for (const auto& element : selected) {
    std::optional<AttributeWritebackTarget> target = captureAttributeWritebackTarget(element);
    if (target.has_value()) {
      if (std::optional<TextPatch> patch = buildElementRemoveWriteback(currentSourceText, *target);
          patch.has_value()) {
        removePatches.push_back(std::move(*patch));
      }
    }
    writebackTargets.push_back(std::move(target));
  }

  std::string sourceAfterDelete(currentSourceText);
  const ApplyPatchesResult patchResult = applyPatches(sourceAfterDelete, removePatches);
  if (!removePatches.empty() && patchResult.applied == removePatches.size() &&
      patchResult.rejectedBounds == 0 && sourceAfterDelete != currentSourceText) {
    const char* label = selected.size() == 1u ? "Delete element" : "Delete elements";
    undoTimeline_.record(label, captureDocumentSourceSnapshot(selected.front(), currentSourceText),
                         captureDocumentSourceSnapshot(selected.front(), sourceAfterDelete));
  }

  // Locked elements survive the delete and stay selected.
  if (lockedSelection.empty()) {
    setSelection(std::nullopt);
  } else {
    setSelection(std::move(lockedSelection));
  }
  for (std::size_t i = 0; i < selected.size(); ++i) {
    if (writebackTargets[i].has_value()) {
      enqueueElementRemoveWriteback(CompletedElementRemoveWriteback{
          .target = std::move(*writebackTargets[i]),
      });
    }
    applyMutation(EditorCommand::DeleteElementCommand(selected[i]));
  }

  return true;
}

bool EditorApp::reorderSelectedElement(ZOrder direction) {
  if (selection_.size() != 1u) {
    return false;
  }
  svg::SVGElement element = selection_.front();
  if (IsLocked(element)) {
    return false;  // Locked elements (or descendants of a locked group) don't move.
  }
  const std::optional<svg::SVGElement> parentOpt = element.parentElement();
  if (!parentOpt.has_value()) {
    return false;  // The document root has no siblings to reorder among.
  }
  const svg::SVGElement parent = *parentOpt;

  // Compute the insert-before reference sibling for the requested move.
  // `std::nullopt` reference means "append" (move to the last sibling). SVG
  // paints in document order, so the last sibling is on top.
  std::optional<svg::SVGElement> referenceElement;
  bool moves = false;
  switch (direction) {
    case ZOrder::BringToFront:
      moves = element.nextSibling().has_value();  // no-op if already last.
      referenceElement = std::nullopt;
      break;
    case ZOrder::SendToBack: {
      const std::optional<svg::SVGElement> first = parent.firstChild();
      moves = first.has_value() && *first != element;  // no-op if already first.
      referenceElement = first;
      break;
    }
    case ZOrder::BringForward:
      if (const std::optional<svg::SVGElement> next = element.nextSibling(); next.has_value()) {
        referenceElement = next->nextSibling();  // move after `next` (nullopt -> append).
        moves = true;
      }
      break;
    case ZOrder::SendBackward:
      if (const std::optional<svg::SVGElement> prev = element.previousSibling(); prev.has_value()) {
        referenceElement = prev;  // move before the previous sibling.
        moves = true;
      }
      break;
  }
  if (!moves) {
    return false;
  }

  return applyElementMove(element, parent, referenceElement, "Reorder element");
}

bool EditorApp::reorderElementBeforeSibling(svg::SVGElement element,
                                            std::optional<svg::SVGElement> referenceSibling) {
  if (IsLocked(element)) {
    return false;  // Locked elements (or descendants of a locked group) don't move.
  }
  const std::optional<svg::SVGElement> parentOpt = element.parentElement();
  if (!parentOpt.has_value()) {
    return false;  // The document root has no siblings to reorder among.
  }
  const svg::SVGElement parent = *parentOpt;

  if (referenceSibling.has_value()) {
    if (*referenceSibling == element) {
      return false;  // Inserting before yourself is a no-op.
    }
    const std::optional<svg::SVGElement> refParent = referenceSibling->parentElement();
    if (!refParent.has_value() || *refParent != parent) {
      return false;  // Cross-parent moves are unsupported here.
    }
  }
  // No-op when the element already sits immediately before the reference (or is
  // already the last child when appending).
  if (element.nextSibling() == referenceSibling) {
    return false;
  }

  return applyElementMove(element, parent, referenceSibling, "Reorder element");
}

bool EditorApp::applyElementMove(svg::SVGElement element, svg::SVGElement parent,
                                 std::optional<svg::SVGElement> referenceElement,
                                 std::string_view undoLabel) {
  svg::SVGDocument& doc = document_.document();
  if (doc.hasSourceStore()) {
    UndoSnapshot before = captureDocumentSourceSnapshot(element, doc.source());
    before.selectionTargets = CaptureSelectionTargets(selection_);
    pendingDocumentSourceUndo_ = PendingDocumentSourceUndo{
        .label = std::string(undoLabel),
        .before = std::move(before),
    };
  }

  // A pure DOM move: `insertElement` re-parents/repositions the already-attached
  // element, and the structured-editing reflection rewrites the source from the
  // DOM change. No source-text surgery (CLAUDE.md "DOM-Level Editing Only").
  applyMutation(EditorCommand::InsertElementCommand(parent, element, referenceElement));
  return true;
}

bool EditorApp::renameSelectedElement(std::string_view newId) {
  if (selection_.size() != 1u) {
    return false;
  }
  svg::SVGElement element = selection_.front();
  if (IsLocked(element)) {
    return false;
  }
  const std::string oldId(element.id().str());
  const std::string newIdStr(newId);
  if (newIdStr.empty() || newIdStr == oldId) {
    return false;
  }

  svg::SVGDocument& doc = document_.document();
  // Reject a collision with a DIFFERENT element (renaming to your own id is the
  // no-op above; an empty oldId means nothing references it yet, which is fine).
  if (const std::optional<svg::SVGElement> existing = doc.querySelector("#" + newIdStr);
      existing.has_value() && *existing != element) {
    return false;
  }

  // Collect every reference that must be repointed, DOM-level: walk all elements
  // and every attribute value, rewriting `url(#oldId)` / `href="#oldId"`. Only
  // meaningful when the element already had an id for things to reference.
  struct PendingAttr {
    svg::SVGElement element;
    std::string name;
    std::string value;
  };
  struct PendingStyle {
    svg::SVGStyleElement element;
    std::string text;
  };
  std::vector<PendingAttr> referenceUpdates;
  std::vector<PendingStyle> styleUpdates;
  if (!oldId.empty()) {
    std::vector<svg::SVGElement> all;
    CollectElements(doc.svgElement(), all);
    for (const svg::SVGElement& candidate : all) {
      for (const xml::XMLQualifiedNameRef& attrName : candidate.attributes()) {
        // We round-trip the rewritten attribute through SetAttributeCommand,
        // which only carries an unprefixed local name. The references we care
        // about (url(#...) in presentation/style attributes, SVG2 `href`) are all
        // in the default namespace; skip prefixed attributes (e.g. legacy
        // `xlink:href`) rather than risk dropping their prefix.
        if (!attrName.namespacePrefix.empty()) {
          continue;
        }
        const std::optional<RcString> attrValue = candidate.getAttribute(attrName);
        if (!attrValue.has_value()) {
          continue;
        }
        if (std::optional<std::string> rewritten =
                RewriteIdReferenceInValue(attrValue->str(), oldId, newIdStr);
            rewritten.has_value()) {
          referenceUpdates.push_back(PendingAttr{
              .element = candidate,
              .name = std::string(attrName.name.str()),
              .value = std::move(*rewritten),
          });
        }
      }

      // Repoint `#oldId` CSS id selectors inside any `<style>` element's text
      // content (DOM-level: read the live stylesheet text, rewrite the selector
      // tokens, and write it back via a SetTextContent command).
      if (candidate.isa<svg::SVGStyleElement>()) {
        svg::SVGStyleElement style = candidate.cast<svg::SVGStyleElement>();
        if (std::optional<std::string> rewritten =
                RewriteIdSelectorInStyle(style.textContent().str(), oldId, newIdStr);
            rewritten.has_value()) {
          styleUpdates.push_back(PendingStyle{
              .element = style,
              .text = std::move(*rewritten),
          });
        }
      }
    }
  }

  if (doc.hasSourceStore()) {
    UndoSnapshot before = captureDocumentSourceSnapshot(element, doc.source());
    before.selectionTargets = CaptureSelectionTargets(selection_);
    pendingDocumentSourceUndo_ = PendingDocumentSourceUndo{
        .label = "Rename element",
        .before = std::move(before),
    };
  }

  applyMutation(EditorCommand::SetAttributeCommand(element, "id", newIdStr));
  for (PendingAttr& update : referenceUpdates) {
    applyMutation(EditorCommand::SetAttributeCommand(update.element, std::move(update.name),
                                                     std::move(update.value)));
  }
  for (PendingStyle& update : styleUpdates) {
    applyMutation(EditorCommand::SetTextContentCommand(update.element, std::move(update.text)));
  }
  return true;
}

void EditorApp::setSelection(std::optional<svg::SVGElement> element) {
  selection_.clear();
  if (element.has_value()) {
    selection_.push_back(std::move(*element));
  }
  refreshFirstSelectionCache();
}

void EditorApp::setSelection(std::vector<svg::SVGElement> elements) {
  selection_ = std::move(elements);
  refreshFirstSelectionCache();
}

bool IsLockGatedCommand(const EditorCommand& command) {
  // Only geometry-changing / destructive mutations are gated. A SetTransform or
  // DeleteElement targeting a locked element (or a descendant of a locked
  // group, via `IsLocked`'s ancestor walk) is dropped. Visibility/lock
  // attribute toggles flow through SetAttribute and are intentionally never
  // gated, so a locked layer can still be shown/hidden and unlocked.
  switch (command.kind) {
    case EditorCommand::Kind::SetTransform:
    case EditorCommand::Kind::DeleteElement:
      return command.element.has_value() && IsLocked(*command.element);
    default: return false;
  }
}

void EditorApp::setElementVisible(const svg::SVGElement& element, bool visible) {
  // Toggle the `display` presentation attribute. Hiding writes
  // `display="none"`; showing writes `display="inline"` (a definitively
  // visible value) so the change is observable through the computed-style
  // visibility check regardless of whether the surrounding stylesheet sets
  // display.
  applyMutation(
      EditorCommand::SetAttributeCommand(element, "display", visible ? "inline" : "none"));
}

void EditorApp::setElementLocked(const svg::SVGElement& element, bool locked) {
  // Toggle the `data-donner-locked` marker. Locking writes `"true"`; unlocking
  // *removes* the attribute entirely (rather than leaving `data-donner-locked
  // ="false"` behind), so an unlocked element looks the same as one that was
  // never locked. This mutation is never lock-gated (see `IsLockGatedCommand`)
  // so a locked layer can always be unlocked.
  if (locked) {
    applyMutation(EditorCommand::SetAttributeCommand(element, std::string(kLockedAttributeName),
                                                     std::string(kLockedAttributeValue)));
  } else {
    applyMutation(
        EditorCommand::RemoveAttributeCommand(element, std::string(kLockedAttributeName)));
  }
}

void EditorApp::toggleInSelection(const svg::SVGElement& element) {
  // SVGElement equality compares the underlying entt handle, so a
  // linear scan is correct (and fine for the typical N ≤ 100 case).
  for (auto it = selection_.begin(); it != selection_.end(); ++it) {
    if (*it == element) {
      selection_.erase(it);
      refreshFirstSelectionCache();
      return;
    }
  }
  selection_.push_back(element);
  refreshFirstSelectionCache();
}

void EditorApp::addToSelection(const svg::SVGElement& element) {
  for (const auto& existing : selection_) {
    if (existing == element) {
      return;
    }
  }
  selection_.push_back(element);
  refreshFirstSelectionCache();
}

bool EditorApp::setAttributeOnSelection(std::string_view attrName, std::string_view attrValue) {
  if (selection_.empty()) {
    return false;
  }

  for (const svg::SVGElement& element : selection_) {
    applyMutation(
        EditorCommand::SetAttributeCommand(element, std::string(attrName), std::string(attrValue)));
  }
  return true;
}

bool EditorApp::setStylePropertyOnSelection(std::string_view propertyName,
                                            std::string_view propertyValue) {
  if (selection_.empty()) {
    return false;
  }

  bool queuedMutation = false;
  for (const svg::SVGElement& element : selection_) {
    const std::optional<RcString> styleAttribute = element.getAttribute("style");
    const std::string_view existingStyle =
        styleAttribute.has_value() ? std::string_view(*styleAttribute) : std::string_view();
    const std::optional<std::string> mergedStyle =
        MergeStyleProperty(existingStyle, propertyName, propertyValue);
    if (!mergedStyle.has_value()) {
      continue;
    }

    applyMutation(EditorCommand::SetAttributeCommand(element, "style", *mergedStyle));
    queuedMutation = true;
  }
  return queuedMutation;
}

bool EditorApp::setStrokeWidthOnSelection(double strokeWidth) {
  const double clampedStrokeWidth = std::max(0.0, strokeWidth);
  return setStylePropertyOnSelection("stroke-width",
                                     donner::detail::FormatNumberForSVG(clampedStrokeWidth));
}

void EditorApp::setActiveStrokeWidth(double strokeWidth) {
  activePaintStyle_.strokeWidth = std::max(0.0, strokeWidth);
}

PathOperationAvailability EditorApp::pathOperationAvailability(PathOperationKind operation) const {
  if (!document_.hasDocument()) {
    return {.canApply = false, .reason = "No SVG document is loaded"};
  }

  if (document_.hasPendingMutations()) {
    return {.canApply = false, .reason = "Document edits are still applying"};
  }

  if (selection_.size() < 2u) {
    return {.canApply = false, .reason = "Select at least two path-convertible elements"};
  }

  const std::vector<svg::SVGElement> selected =
      SortSelectionByPaintOrder(document_.document(), selection_);
  if (selected.size() != selection_.size()) {
    return {.canApply = false, .reason = "Selection includes detached geometry"};
  }

  const PathOperationSelection pathSelection = CollectPathOperationSelection(selected);
  if (pathSelection.inputs.size() != selection_.size()) {
    return {.canApply = false, .reason = "Selection includes unsupported or empty geometry"};
  }

  if (operation == PathOperationKind::Intersect &&
      !InputIntersectionBounds(pathSelection).has_value()) {
    return {.canApply = false, .reason = "Selected path bounds do not overlap"};
  }

  return {.canApply = true};
}

bool EditorApp::applyPathOperation(PathOperationKind operation) {
  const PathOperationAvailability availability = pathOperationAvailability(operation);
  if (!availability.canApply) {
    return false;
  }

  svg::SVGDocument& document = document_.document();
  const std::vector<svg::SVGElement> selected = SortSelectionByPaintOrder(document, selection_);
  if (selected.size() != selection_.size()) {
    return false;
  }

  const PathOperationSelection pathSelection = CollectPathOperationSelection(selected);
  const std::vector<PathBooleanInput> booleanInputs =
      InputsForEditorOperation(operation, pathSelection);
  const PathBooleanResult booleanResult = ApplyPathBoolean(
      BooleanOpForEditorOperation(operation), booleanInputs, EditorPathBooleanOptions());
  if (booleanResult.status != PathBooleanStatus::Ok || booleanResult.paths.empty()) {
    return false;
  }

  const Path resultDocumentSpline = CombineBooleanPaths(booleanResult.paths);
  if (resultDocumentSpline.empty()) {
    return false;
  }
  if (!PathOperationResultFitsInputBounds(resultDocumentSpline, pathSelection)) {
    return false;
  }

  const svg::SVGElement baseElement = BaseElementForPathOperation(operation, selected);
  svg::SVGElement parent = baseElement.parentElement().value_or(document.svgElement());
  Transform2d parentFromDocument;
  if (parent.isa<svg::SVGGraphicsElement>()) {
    const Transform2d documentFromParent =
        parent.cast<svg::SVGGraphicsElement>().elementFromWorld();
    parentFromDocument = documentFromParent.inverse();
  }

  const Path resultSpline = TransformPath(resultDocumentSpline, parentFromDocument);
  const RcString resultPathData = resultSpline.toSVGPathData();
  svg::SVGPathElement resultPath = svg::SVGPathElement::Create(document);
  resultPath.setAttribute("d", std::string_view(resultPathData));
  CopyPathOperationStyle(baseElement, resultPath);

  if (document.hasSourceStore()) {
    UndoSnapshot before = captureDocumentSourceSnapshot(baseElement, document.source());
    before.selectionTargets = CaptureSelectionTargets(selected);
    pendingDocumentSourceUndo_ = PendingDocumentSourceUndo{
        .label = "Path operation",
        .before = std::move(before),
    };
  }

  std::optional<svg::SVGElement> referenceElement;
  if (const std::optional<svg::SVGElement> selectedParent = baseElement.parentElement();
      selectedParent.has_value() && *selectedParent == parent) {
    referenceElement = baseElement;
  }

  applyMutation(EditorCommand::InsertElementCommand(parent, resultPath, referenceElement));
  for (const svg::SVGElement& element : selected) {
    applyMutation(EditorCommand::DeleteElementCommand(element));
  }
  setSelection(resultPath);
  return true;
}

PathOperationAvailability EditorApp::compoundPathUnbundleAvailability(
    std::optional<svg::SVGElement> target) const {
  if (!document_.hasDocument()) {
    return {.canApply = false, .reason = "No SVG document is loaded"};
  }

  if (document_.hasPendingMutations()) {
    return {.canApply = false, .reason = "Document edits are still applying"};
  }

  std::optional<svg::SVGElement> resolvedTarget = ResolveCompoundPathUnbundleTarget(*this, target);
  if (!resolvedTarget.has_value()) {
    return {.canApply = false, .reason = "Select one compound path"};
  }

  if (!resolvedTarget->isa<svg::SVGPathElement>()) {
    return {.canApply = false, .reason = "Target is not a path"};
  }

  if (!resolvedTarget->parentElement().has_value()) {
    return {.canApply = false, .reason = "Target path is detached"};
  }

  const svg::SVGPathElement pathElement = resolvedTarget->cast<svg::SVGPathElement>();
  const std::optional<Path> spline = pathElement.computedSpline();
  if (!spline.has_value() || spline->empty()) {
    return {.canApply = false, .reason = "Path has no geometry"};
  }

  const CompoundPathSplit split = SplitCompoundPathIntoContours(*spline);
  if (split.components.size() < 2u) {
    return {.canApply = false, .reason = "Path has one contour"};
  }

  return {.canApply = true};
}

bool EditorApp::unbundleCompoundPath(std::optional<svg::SVGElement> target) {
  const PathOperationAvailability availability = compoundPathUnbundleAvailability(target);
  if (!availability.canApply) {
    return false;
  }

  svg::SVGDocument& document = document_.document();
  std::optional<svg::SVGElement> resolvedTarget = ResolveCompoundPathUnbundleTarget(*this, target);
  if (!resolvedTarget.has_value() || !resolvedTarget->isa<svg::SVGPathElement>()) {
    return false;
  }

  const svg::SVGPathElement sourcePath = resolvedTarget->cast<svg::SVGPathElement>();
  const std::optional<Path> sourceSpline = sourcePath.computedSpline();
  if (!sourceSpline.has_value()) {
    return false;
  }

  const CompoundPathSplit split = SplitCompoundPathIntoContours(*sourceSpline);
  if (split.components.size() < 2u) {
    return false;
  }

  svg::SVGElement parent = sourcePath.parentElement().value_or(document.svgElement());
  std::vector<svg::SVGElement> replacementSelection;
  replacementSelection.reserve(split.components.size());

  if (document.hasSourceStore()) {
    UndoSnapshot before = captureDocumentSourceSnapshot(sourcePath, document.source());
    const std::array<svg::SVGElement, 1> sourceSelection = {sourcePath};
    before.selectionTargets = CaptureSelectionTargets(sourceSelection);
    pendingDocumentSourceUndo_ = PendingDocumentSourceUndo{
        .label = "Unbundle compound path",
        .before = std::move(before),
    };
  }

  for (const Path& component : split.components) {
    svg::SVGPathElement replacement = svg::SVGPathElement::Create(document);
    const RcString pathData = component.toSVGPathData();
    replacement.setAttribute("d", std::string_view(pathData));
    CopyUnbundleAttributes(sourcePath, replacement);
    replacementSelection.push_back(replacement);
    applyMutation(EditorCommand::InsertElementCommand(parent, replacement, sourcePath));
  }
  applyMutation(EditorCommand::DeleteElementCommand(sourcePath));

  setSelection(std::move(replacementSelection));
  return true;
}

void EditorApp::recordDocumentSourceUndoOnNextFlush(std::string label,
                                                    svg::SVGElement anchorElement,
                                                    std::string beforeSource) {
  pendingDocumentSourceUndo_ = PendingDocumentSourceUndo{
      .label = std::move(label),
      .before = captureDocumentSourceSnapshot(anchorElement, beforeSource),
  };
}

void EditorApp::refreshFirstSelectionCache() {
  if (selection_.empty()) {
    cachedFirstSelection_.reset();
  } else {
    cachedFirstSelection_ = selection_.front();
  }
}

void EditorApp::undo() {
  auto snapshot = undoTimeline_.undo();
  if (!snapshot.has_value()) {
    return;
  }

  if (snapshot->kind == UndoSnapshot::Kind::DocumentSource) {
    pendingTransformWritebacks_.clear();
    pendingElementRemoveWritebacks_.clear();
  }
  ApplyTimelineSnapshot(*this, document_, *snapshot);
}

void EditorApp::redo() {
  auto snapshot = undoTimeline_.redo();
  if (!snapshot.has_value()) {
    return;
  }

  if (snapshot->kind == UndoSnapshot::Kind::DocumentSource) {
    pendingTransformWritebacks_.clear();
    pendingElementRemoveWritebacks_.clear();
  }
  ApplyTimelineSnapshot(*this, document_, *snapshot);
}

std::optional<svg::SVGGraphicsElement> EditorApp::hitTest(const Vector2d& documentPoint) {
  if (!document_.hasDocument()) {
    return std::nullopt;
  }

  // Rebuild the DonnerController whenever the document version advances
  // past the version we built it for. The controller copies the SVGDocument
  // handle (which internally shares the registry), so reconstruction is
  // cheap relative to a full re-parse.
  const auto currentVersion = document_.currentFrameVersion();
  if (!controller_.has_value() || controllerVersion_ != currentVersion) {
    controller_.emplace(document_.document());
    controllerVersion_ = currentVersion;
  }

  return controller_->findIntersecting(documentPoint);
}

std::vector<svg::SVGGraphicsElement> EditorApp::hitTestRect(const Box2d& documentRect) {
  std::vector<svg::SVGGraphicsElement> hits;
  if (!document_.hasDocument()) {
    return hits;
  }

  // Walk the live document and collect every geometry element whose filled or stroked path
  // intersects the marquee rect. We don't go through `DonnerController` because it's point-only;
  // the linear walk is simple and fine for typical editor workloads.
  //
  // §concurrent-dom: the editor keeps the live document in ConcurrentDom, so this UI-thread walk
  // needs a scoped access guard or its DOM reads (isa / firstChild / nextSibling) trip the
  // `ElementAnchor` release assertion. GeometryIntersectsRect() calls worldBounds(), which lazily
  // computes shape state under *write* access, so the whole traversal takes one coarse write guard.
  svg::SVGDocument doc = document_.document();
  doc.withWriteAccess([&](svg::DocumentWriteAccess&) {
    const svg::SVGElement root = doc.svgElement();
    auto visit = [&](const svg::SVGGraphicsElement& element) {
      if (element.isa<svg::SVGGeometryElement>()) {
        if (GeometryIntersectsRect(element.cast<svg::SVGGeometryElement>(), documentRect)) {
          hits.push_back(element);
        }
      } else if (element.isa<svg::SVGTextElement>()) {
        if (TextIntersectsRect(element.cast<svg::SVGTextElement>(), documentRect)) {
          hits.push_back(element);
        }
      }
    };
    ForEachSelectableElement(root, visit);
  });
  return hits;
}

std::vector<svg::SVGElement> EditorApp::selectableElements() {
  std::vector<svg::SVGElement> selectable;
  if (!document_.hasDocument()) {
    return selectable;
  }

  // Mirror `hitTestRect`'s traversal exactly so the "Select All" set and marquee selection agree
  // on what is selectable: every geometry element and `<text>` root in the tree, in document
  // order. Non-selectable nodes (`<defs>`, gradients, plain containers, XML text nodes) are
  // skipped by `ForEachSelectableElement`.
  //
  // §concurrent-dom: like `hitTestRect`, the DOM reads (isa / firstChild / nextSibling) need a
  // scoped access guard against the live ConcurrentDom document.
  svg::SVGDocument doc = document_.document();
  doc.withWriteAccess([&](svg::DocumentWriteAccess&) {
    const svg::SVGElement root = doc.svgElement();
    auto visit = [&](const svg::SVGGraphicsElement& element) { selectable.emplace_back(element); };
    ForEachSelectableElement(root, visit);
  });
  return selectable;
}

}  // namespace donner::editor
