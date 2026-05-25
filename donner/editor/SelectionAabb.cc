#include "donner/editor/SelectionAabb.h"

#include <optional>

#include "donner/base/xml/XMLNode.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"

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
    // for outline purposes — stop here.
    return;
  }
  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    CollectRenderableGeometryImpl(current, out);
  }
}

void CollectLaterRenderableGeometryImpl(const svg::SVGElement& root,
                                        const svg::SVGElement& selected, bool& afterSelected,
                                        std::vector<svg::SVGGeometryElement>& out) {
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
      out.push_back(root.cast<svg::SVGGeometryElement>());
    }
    return;
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    CollectLaterRenderableGeometryImpl(current, selected, afterSelected, out);
  }
}

std::vector<svg::SVGGeometryElement> CollectLaterRenderableGeometry(
    const svg::SVGElement& root, const svg::SVGElement& selected) {
  std::vector<svg::SVGGeometryElement> out;
  bool afterSelected = false;
  CollectLaterRenderableGeometryImpl(root, selected, afterSelected, out);
  return out;
}

std::vector<Box2d> SnapshotGeometryWorldBounds(
    std::span<const svg::SVGGeometryElement> geometryElements) {
  std::vector<Box2d> bounds;
  bounds.reserve(geometryElements.size());
  for (const auto& geometry : geometryElements) {
    const auto wb = geometry.worldBounds();
    if (wb.has_value()) {
      bounds.push_back(*wb);
    }
  }
  return bounds;
}

}  // namespace

std::vector<svg::SVGGeometryElement> CollectRenderableGeometry(const svg::SVGElement& root) {
  std::vector<svg::SVGGeometryElement> out;
  CollectRenderableGeometryImpl(root, out);
  return out;
}

std::vector<Box2d> SnapshotSelectionWorldBounds(std::span<const svg::SVGElement> selection) {
  std::vector<Box2d> bounds;
  bounds.reserve(selection.size());
  for (const auto& element : selection) {
    // Expand each selection entry to its renderable-geometry leaves and
    // union their world bounds. For a plain geometry selection this is
    // a single-element collection that reduces to `worldBounds()`; for a
    // `<g filter>` it unions every descendant shape so the AABB
    // envelopes the visible group.
    std::optional<Box2d> merged;
    for (const auto& geometry : CollectRenderableGeometry(element)) {
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
  if (!HasLiveSvgTreeComponents(selected)) {
    return {};
  }

  const svg::SVGElement root = selected.ownerDocument().svgElement();
  const std::vector<svg::SVGGeometryElement> laterGeometry =
      CollectLaterRenderableGeometry(root, selected);
  return SnapshotGeometryWorldBounds(laterGeometry);
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
