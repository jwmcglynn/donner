#include "donner/editor/LayerTreeModel.h"

#include <algorithm>
#include <string>
#include <vector>

#include "donner/svg/ElementType.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {

namespace {

/// True for element types that are renderable leaf shapes in the editor layer
/// tree. Compound `<path>` elements are also leaves but are classified
/// separately (see `ClassifyKind`).
bool IsRenderableLeafType(svg::ElementType type) {
  switch (type) {
    case svg::ElementType::Path:
    case svg::ElementType::Rect:
    case svg::ElementType::Circle:
    case svg::ElementType::Ellipse:
    case svg::ElementType::Line:
    case svg::ElementType::Polyline:
    case svg::ElementType::Polygon:
    case svg::ElementType::Text:
    case svg::ElementType::Image:
    case svg::ElementType::Use: return true;
    default: return false;
  }
}

/// True for grouping containers that can hold renderable descendants.
bool IsGroupType(svg::ElementType type) {
  return type == svg::ElementType::G || type == svg::ElementType::SVG;
}

/// True for non-rendered resource subtrees that are excluded from the default
/// Layers panel (design doc 0046 "Requirements and Constraints").
bool IsExcludedResourceType(svg::ElementType type) {
  switch (type) {
    case svg::ElementType::Defs:
    case svg::ElementType::LinearGradient:
    case svg::ElementType::RadialGradient:
    case svg::ElementType::Pattern:
    case svg::ElementType::Filter:
    case svg::ElementType::ClipPath:
    case svg::ElementType::Mask:
    case svg::ElementType::Marker:
    case svg::ElementType::Symbol:
    case svg::ElementType::Style:
    case svg::ElementType::Stop: return true;
    default: return false;
  }
}

/// True when `element` has at least one editor-visible descendant. Forward-
/// declared because it is mutually recursive with `IsEditorVisible` (a group is
/// only editor-visible when it has a renderable descendant).
bool HasRenderableDescendant(const svg::SVGElement& element);

/// Whether `element` should appear as a row in the layer tree. The root `<svg>`
/// always appears; group containers appear when they have renderable
/// descendants; renderable leaves appear; everything else is excluded.
bool IsEditorVisible(const svg::SVGElement& element) {
  const std::optional<svg::ElementType> type = element.tryType();
  if (!type.has_value()) {
    return false;
  }
  if (IsExcludedResourceType(*type)) {
    return false;
  }
  if (*type == svg::ElementType::SVG) {
    return true;
  }
  if (IsGroupType(*type)) {
    return HasRenderableDescendant(element);
  }
  return IsRenderableLeafType(*type);
}

bool HasRenderableDescendant(const svg::SVGElement& element) {
  for (std::optional<svg::SVGElement> child = element.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (IsEditorVisible(*child)) {
      return true;
    }
  }
  return false;
}

/// Count the number of subpaths in a path `d` string by counting move-to
/// commands (`M`/`m`). A path with more than one subpath is a compound path.
/// Counting tokens in the source `d` is deterministic and avoids depending on
/// the computed-spline command API; the showcase splash and the model tests
/// both exercise multi-`M` compound paths.
bool IsCompoundPath(const svg::SVGElement& element) {
  if (element.type() != svg::ElementType::Path) {
    return false;
  }
  const svg::SVGPathElement pathElement = element.cast<svg::SVGPathElement>();
  const RcString d = pathElement.d();
  const std::string_view dView = d;
  int moveToCount = 0;
  for (const char c : dView) {
    if (c == 'M' || c == 'm') {
      ++moveToCount;
    }
  }
  return moveToCount > 1;
}

/// Classify an editor-visible element into a `LayerRowKind`.
LayerRowKind ClassifyKind(const svg::SVGElement& element) {
  const svg::ElementType type = element.type();
  if (type == svg::ElementType::SVG) {
    return LayerRowKind::Root;
  }
  if (IsGroupType(type)) {
    return LayerRowKind::Group;
  }
  if (type == svg::ElementType::Path && IsCompoundPath(element)) {
    return LayerRowKind::CompoundPath;
  }
  if (IsRenderableLeafType(type)) {
    return LayerRowKind::Shape;
  }
  return LayerRowKind::Other;
}

/// Build a display name: the element `id` if non-empty, otherwise
/// `<tag>[n]` where `n` is the element's zero-based index among its siblings of
/// the same tag.
std::string BuildDisplayName(const svg::SVGElement& element) {
  const RcString id = element.id();
  const std::string_view idView = id;
  if (!idView.empty()) {
    return std::string(idView);
  }

  // Keep the qualified name alive so the .name string_view does not dangle (see
  // SidebarPresenter::BuildTreeNodeLabel for the same hazard).
  const xml::XMLQualifiedNameRef qualifiedName = element.tagName();
  const std::string_view tagView = qualifiedName.name;

  int sameTagIndex = 0;
  const std::optional<svg::SVGElement> parent = element.parentElement();
  std::optional<svg::SVGElement> sibling =
      parent.has_value() ? parent->firstChild() : std::optional<svg::SVGElement>{};
  for (; sibling.has_value(); sibling = sibling->nextSibling()) {
    if (*sibling == element) {
      break;
    }
    const xml::XMLQualifiedNameRef siblingName = sibling->tagName();
    if (siblingName.name == tagView) {
      ++sameTagIndex;
    }
  }

  std::string name;
  name.push_back('<');
  name.append(tagView.data(), tagView.size());
  name.push_back('>');
  name.push_back('[');
  name.append(std::to_string(sameTagIndex));
  name.push_back(']');
  return name;
}

bool IsVisible(const svg::SVGElement& element) {
  const svg::PropertyRegistry& style = element.getComputedStyle();
  if (const auto display = style.display.get();
      display.has_value() && *display == svg::Display::None) {
    return false;
  }
  if (const auto visibility = style.visibility.get();
      visibility.has_value() && *visibility != svg::Visibility::Visible) {
    return false;
  }
  return true;
}

bool IsInSelection(const std::vector<svg::SVGElement>& selection, const svg::SVGElement& element) {
  return std::find(selection.begin(), selection.end(), element) != selection.end();
}

/// True when any (transitive) descendant of `element` is in `selection`.
bool HasSelectedDescendant(const svg::SVGElement& element,
                           const std::vector<svg::SVGElement>& selection) {
  for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
    if (IsInSelection(selection, *child) || HasSelectedDescendant(*child, selection)) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::uint64_t LayerTreeModel::StableIdFor(const svg::SVGElement& element) {
  const Entity entity = element.unsafeEntityHandle().entity();
  return static_cast<std::uint64_t>(static_cast<std::uint32_t>(entity));
}

void LayerTreeModel::setExpanded(std::uint64_t stableId, bool expanded) {
  if (expanded) {
    expanded_.insert(stableId);
  } else {
    expanded_.erase(stableId);
  }
}

void LayerTreeModel::toggleExpanded(std::uint64_t stableId) {
  if (expanded_.count(stableId) != 0) {
    expanded_.erase(stableId);
  } else {
    expanded_.insert(stableId);
  }
}

bool LayerTreeModel::isExpanded(std::uint64_t stableId) const {
  return expanded_.count(stableId) != 0;
}

void LayerTreeModel::appendRows(const svg::SVGElement& element, int depth,
                                const std::vector<svg::SVGElement>& selection) {
  LayerTreeRow row{
      .depth = depth,
      .displayName = BuildDisplayName(element),
      .stableId = StableIdFor(element),
      .element = element,
      .hasChildren = HasRenderableDescendant(element),
      .isExpanded = false,
      .isVisible = IsVisible(element),
      .isSelected = IsInSelection(selection, element),
      .isPartiallySelected = false,
      .kind = ClassifyKind(element),
  };

  const bool isGroupLike = row.kind == LayerRowKind::Root || row.kind == LayerRowKind::Group;
  if (isGroupLike && !row.isSelected) {
    row.isPartiallySelected = HasSelectedDescendant(element, selection);
  }
  row.isExpanded = row.hasChildren && isExpanded(row.stableId);
  rows_.push_back(std::move(row));

  if (!isExpanded(StableIdFor(element)) || !HasRenderableDescendant(element)) {
    return;
  }

  // Visual stack order: later-painted siblings appear above earlier-painted
  // ones, so emit editor-visible children in reverse document order.
  std::vector<svg::SVGElement> children;
  for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
    if (IsEditorVisible(*child)) {
      children.push_back(*child);
    }
  }
  for (auto it = children.rbegin(); it != children.rend(); ++it) {
    appendRows(*it, depth + 1, selection);
  }
}

void LayerTreeModel::refresh(const EditorApp& app) {
  rows_.clear();
  if (!app.hasDocument()) {
    return;
  }

  // Mirror SidebarPresenter's snapshot discipline: hold document write access
  // for the duration of the tree walk so concurrent-DOM reads are guarded.
  [[maybe_unused]] const svg::DocumentWriteAccess snapshotWriteAccess =
      app.document().document().writeAccess();

  const svg::SVGElement root = app.document().document().svgElement();

  // The Layers panel omits the document root `<svg>` row entirely and shows its
  // editor-visible children (top-level groups and shapes) as the tree's roots.
  std::vector<svg::SVGElement> topLevel;
  for (std::optional<svg::SVGElement> child = root.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (IsEditorVisible(*child)) {
      topLevel.push_back(*child);
    }
  }

  if (!initialized_) {
    // Default-expand the top-level groups exactly once so the first tier of
    // group contents is visible on open, without re-expanding after a user
    // collapse.
    for (const svg::SVGElement& child : topLevel) {
      if (child.tryType() == svg::ElementType::G && HasRenderableDescendant(child)) {
        expanded_.insert(StableIdFor(child));
      }
    }
    initialized_ = true;
  }

  // Visual stack order: later-painted siblings appear above earlier-painted
  // ones, so emit the top-level rows in reverse document order.
  const std::vector<svg::SVGElement> selection = app.selectedElements();
  for (auto it = topLevel.rbegin(); it != topLevel.rend(); ++it) {
    appendRows(*it, /*depth=*/0, selection);
  }
}

}  // namespace donner::editor
