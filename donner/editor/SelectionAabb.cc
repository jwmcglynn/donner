#include "donner/editor/SelectionAabb.h"

#include <array>
#include <optional>

#include "donner/base/Transform.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGTextElement.h"

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
    // Text content (tspans) contributes chrome through its root — stop here.
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

/// Merge the document-space bounds of every renderable leaf (geometry world
/// bounds + text ink bounds) in @p root's subtree into @p merged.
void MergeRenderableWorldBounds(const svg::SVGElement& root, std::optional<Box2d>& merged) {
  std::vector<svg::SVGGeometryElement> geometryElements;
  CollectRenderableGeometryImpl(root, geometryElements);
  for (const auto& geometry : geometryElements) {
    const auto wb = geometry.worldBounds();
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
    const std::optional<Box2d> inkDoc = TextWorldInkBounds(text);
    if (!inkDoc.has_value()) {
      continue;
    }
    if (merged.has_value()) {
      merged->addBox(*inkDoc);
    } else {
      merged = *inkDoc;
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
      const auto wb = root.cast<svg::SVGGeometryElement>().worldBounds();
      if (wb.has_value()) {
        out.push_back(*wb);
      }
    }
    return;
  }

  if (root.isa<svg::SVGTextElement>()) {
    if (afterSelected) {
      const std::optional<Box2d> inkDoc = TextWorldInkBounds(root.cast<svg::SVGTextElement>());
      if (inkDoc.has_value()) {
        out.push_back(*inkDoc);
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

std::optional<Box2d> TextWorldInkBounds(const svg::SVGTextElement& text) {
  const Box2d inkLocal = text.inkBoundingBox();
  if (inkLocal.isEmpty()) {
    return std::nullopt;
  }

  const Transform2d documentFromText = text.elementFromWorld();
  const std::array<Vector2d, 4> corners = {
      inkLocal.topLeft, Vector2d(inkLocal.bottomRight.x, inkLocal.topLeft.y), inkLocal.bottomRight,
      Vector2d(inkLocal.topLeft.x, inkLocal.bottomRight.y)};
  Box2d inkDoc = Box2d::CreateEmpty(documentFromText.transformPosition(corners[0]));
  for (std::size_t i = 1; i < corners.size(); ++i) {
    inkDoc.addPoint(documentFromText.transformPosition(corners[i]));
  }
  return inkDoc;
}

std::vector<Box2d> SnapshotSelectionWorldBounds(std::span<const svg::SVGElement> selection) {
  std::vector<Box2d> bounds;
  bounds.reserve(selection.size());
  for (const auto& element : selection) {
    // Expand each selection entry to its renderable leaves and union their
    // world bounds. For a plain geometry selection this is a single-element
    // collection that reduces to `worldBounds()`; a `<text>` root reduces to
    // its ink bounds; for a `<g filter>` it unions every descendant shape and
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
