#include "donner/editor/StrokeMarkerPrefabs.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/svg/SVGDefsElement.h"
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/graph/Reference.h"

namespace donner::editor {
namespace {

struct MarkerSpec {
  std::string_view key;
  std::string_view path;
  int refX;
  bool open = false;
};

std::optional<MarkerSpec> SpecFor(StrokeMarkerPrefab prefab) {
  switch (prefab) {
    case StrokeMarkerPrefab::FilledArrow: return MarkerSpec{"filled-arrow", "M1 1 L7 4 L1 7 Z", 7};
    case StrokeMarkerPrefab::OpenArrow: return MarkerSpec{"open-arrow", "M1 1 L7 4 L1 7", 7, true};
    case StrokeMarkerPrefab::Dot:
      return MarkerSpec{"dot", "M4 1 A3 3 0 1 0 4 7 A3 3 0 1 0 4 1 Z", 4};
    case StrokeMarkerPrefab::Diamond: return MarkerSpec{"diamond", "M0 4 L4 0 L8 4 L4 8 Z", 8};
  }
  return std::nullopt;
}

bool IsGeneratedPrefabId(std::string_view id, std::string_view key) {
  if (id.empty() || id.size() > 100u) {
    return false;
  }
  const std::string base = "donner-marker-" + std::string(key);
  if (id == base) {
    return true;
  }
  const std::string prefix = base + "-";
  if (!id.starts_with(prefix)) {
    return false;
  }
  const std::string_view suffix = id.substr(prefix.size());
  return !suffix.empty() && suffix.front() >= '1' && suffix.front() <= '9' &&
         std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool AttributeIs(const svg::SVGElement& element, const char* name, std::string_view expected) {
  const std::optional<RcString> value = element.getAttribute(name);
  return value.has_value() && std::string_view(*value) == expected;
}

bool MatchesMarkerAttributes(const svg::SVGElement& marker, const MarkerSpec& spec) {
  const std::string refX = std::to_string(spec.refX);
  return AttributeIs(marker, "data-donner-prefab-marker", spec.key) &&
         AttributeIs(marker, "viewBox", "0 0 8 8") && AttributeIs(marker, "markerWidth", "4") &&
         AttributeIs(marker, "markerHeight", "4") && AttributeIs(marker, "refX", refX) &&
         AttributeIs(marker, "refY", "4") && AttributeIs(marker, "orient", "auto-start-reverse");
}

bool MatchesOpenArrowPaint(const svg::SVGElement& shape) {
  return AttributeIs(shape, "stroke", "context-stroke") &&
         AttributeIs(shape, "stroke-width", "1.4") &&
         AttributeIs(shape, "stroke-linecap", "round") &&
         AttributeIs(shape, "stroke-linejoin", "round");
}

bool MatchesPrefabShape(const svg::SVGElement& shape, const MarkerSpec& spec) {
  const RcString shapeTag = shape.tagName().name;
  if (std::string_view(shapeTag) != "path" || shape.attributes().size() != (spec.open ? 6u : 2u) ||
      !AttributeIs(shape, "d", spec.path) ||
      !AttributeIs(shape, "fill", spec.open ? "none" : "context-stroke")) {
    return false;
  }
  return !spec.open || MatchesOpenArrowPaint(shape);
}

bool MatchesPrefabDefinition(const svg::SVGElement& marker, const MarkerSpec& spec) {
  if (marker.attributes().size() != 8u || !MatchesMarkerAttributes(marker, spec)) {
    return false;
  }
  const std::optional<svg::SVGElement> shape = marker.firstChild();
  if (!shape.has_value() || shape->nextSibling().has_value()) {
    return false;
  }
  return MatchesPrefabShape(*shape, spec);
}

std::optional<std::string> ExistingPrefabId(svg::SVGDocument& document, const MarkerSpec& spec) {
  std::deque<svg::SVGElement> pending{document.svgElement()};
  std::size_t visited = 0;
  while (!pending.empty() && visited++ < 4096u) {
    const svg::SVGElement element = pending.front();
    pending.pop_front();
    const RcString tag = element.tagName().name;
    const std::optional<RcString> preset = element.getAttribute("data-donner-prefab-marker");
    const RcString id = element.id();
    const std::string_view name = id;
    if (std::string_view(tag) == "marker" && preset.has_value() &&
        std::string_view(*preset) == spec.key && IsGeneratedPrefabId(name, spec.key) &&
        MatchesPrefabDefinition(element, spec)) {
      const svg::Reference reference(RcString("#" + std::string(name)));
      const std::optional<svg::ResolvedReference> resolved = reference.resolve(document.registry());
      if (resolved.has_value() &&
          resolved->handle.entity() == element.unsafeEntityHandle().entity()) {
        return std::string(name);
      }
    }
    for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
      if (pending.size() + visited >= 4096u) {
        break;
      }
      pending.push_back(*child);
    }
  }
  return std::nullopt;
}

std::optional<std::string> AvailablePrefabId(svg::SVGDocument& document, std::string_view key) {
  const std::string base = "donner-marker-" + std::string(key);
  for (int suffix = 0; suffix < 1024; ++suffix) {
    const std::string id = suffix == 0 ? base : base + "-" + std::to_string(suffix);
    if (!document.querySelector("#" + id).has_value()) {
      return id;
    }
  }
  return std::nullopt;
}

std::optional<svg::SVGElement> RootDefs(const svg::SVGElement& root) {
  std::size_t visited = 0;
  for (auto child = root.firstChild(); child.has_value() && visited++ < 4096u;
       child = child->nextSibling()) {
    const RcString tag = child->tagName().name;
    if (std::string_view(tag) == "defs") {
      return child;
    }
  }
  return std::nullopt;
}

std::optional<svg::SVGElement> MarkerWithId(const svg::SVGElement& root, std::string_view id) {
  const svg::Reference reference(RcString("#" + std::string(id)));
  const std::optional<svg::ResolvedReference> resolved =
      reference.resolve(*root.unsafeEntityHandle().registry());
  if (!resolved.has_value()) {
    return std::nullopt;
  }
  std::deque<svg::SVGElement> pending{root};
  std::size_t visited = 0;
  while (!pending.empty() && visited++ < 4096u) {
    const svg::SVGElement element = pending.front();
    pending.pop_front();
    const RcString tag = element.tagName().name;
    const RcString elementId = element.id();
    if (std::string_view(tag) == "marker" && std::string_view(elementId) == id &&
        element.unsafeEntityHandle().entity() == resolved->handle.entity()) {
      return element;
    }
    for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
      if (pending.size() + visited >= 4096u) {
        break;
      }
      pending.push_back(*child);
    }
  }
  return std::nullopt;
}

void QueuePrefabDefinition(EditorApp& app, const MarkerSpec& spec, std::string_view id) {
  svg::SVGDocument& document = app.document().document();
  const svg::SVGElement root = document.svgElement();
  std::optional<svg::SVGElement> defs = RootDefs(root);
  if (!defs.has_value()) {
    defs = svg::SVGDefsElement::Create(document);
    app.applyMutation(EditorCommand::InsertElementCommand(root, *defs, root.firstChild()));
  }

  svg::SVGMarkerElement marker = svg::SVGMarkerElement::Create(document);
  marker.setAttribute("id", id);
  marker.setAttribute("data-donner-prefab-marker", spec.key);
  marker.setAttribute("viewBox", "0 0 8 8");
  marker.setAttribute("markerWidth", "4");
  marker.setAttribute("markerHeight", "4");
  const std::string refX = std::to_string(spec.refX);
  marker.setAttribute("refX", refX);
  marker.setAttribute("refY", "4");
  marker.setAttribute("orient", "auto-start-reverse");
  const Box2d viewBox(Vector2d(0, 0), Vector2d(8, 8));
  marker.setViewBox(viewBox);
  marker.setMarkerWidth(Lengthd(4));
  marker.setMarkerHeight(Lengthd(4));
  marker.setRefX(Lengthd(spec.refX));
  marker.setRefY(Lengthd(4));
  marker.setOrient(svg::MarkerOrient::AutoStartReverse());
  app.applyMutation(EditorCommand::InsertElementCommand(*defs, marker));

  svg::SVGPathElement path = svg::SVGPathElement::Create(document);
  path.setAttribute("d", spec.path);
  path.setAttribute("fill", spec.open ? "none" : "context-stroke");
  if (spec.open) {
    path.setAttribute("stroke", "context-stroke");
    path.setAttribute("stroke-width", "1.4");
    path.setAttribute("stroke-linecap", "round");
    path.setAttribute("stroke-linejoin", "round");
  }
  app.applyMutation(EditorCommand::InsertElementCommand(marker, path));
}

}  // namespace

bool ApplyStrokeMarkerPrefab(EditorApp& app, std::string_view property, StrokeMarkerPrefab prefab) {
  if ((property != "marker-start" && property != "marker-end") || !app.hasSelection() ||
      !app.document().hasDocument()) {
    return false;
  }
  const std::optional<MarkerSpec> spec = SpecFor(prefab);
  if (!spec.has_value()) {
    return false;
  }
  svg::SVGDocument& document = app.document().document();
  const std::optional<std::string> existingId = ExistingPrefabId(document, *spec);
  const std::optional<std::string> id =
      existingId.has_value() ? existingId : AvailablePrefabId(document, spec->key);
  if (!id.has_value()) {
    return false;
  }
  const std::string before(document.source());
  const std::string reference = "url(#" + *id + ")";
  // The reference resolves during style application, so its definition must enter the DOM first.
  if (!existingId.has_value()) {
    QueuePrefabDefinition(app, *spec, *id);
  }
  if (!app.setStylePropertyOnSelection(property, reference)) {
    return false;
  }
  app.recordDocumentSourceUndoOnNextFlush("Change stroke marker", document.svgElement(), before,
                                          /*preserveSelection=*/true);
  return true;
}

std::optional<StrokeMarkerPrefab> StrokeMarkerPrefabForReference(svg::SVGDocument& document,
                                                                 std::string_view reference) {
  if (!reference.starts_with("url(#") || !reference.ends_with(')')) {
    return std::nullopt;
  }
  const std::string id(reference.substr(5, reference.size() - 6));
  const std::optional<svg::SVGElement> marker = MarkerWithId(document.svgElement(), id);
  if (!marker.has_value()) {
    return std::nullopt;
  }
  return StrokeMarkerPrefabForElement(*marker);
}

std::optional<StrokeMarkerPrefab> StrokeMarkerPrefabForElement(const svg::SVGElement& marker) {
  const RcString tag = marker.tagName().name;
  const RcString id = marker.id();
  if (std::string_view(tag) != "marker") {
    return std::nullopt;
  }
  const std::optional<RcString> key = marker.getAttribute("data-donner-prefab-marker");
  if (!key.has_value()) {
    return std::nullopt;
  }
  for (const StrokeMarkerPrefabOption& option : kStrokeMarkerPrefabOptions) {
    const std::optional<MarkerSpec> spec = SpecFor(option.prefab);
    if (!spec.has_value() || std::string_view(*key) != spec->key ||
        !IsGeneratedPrefabId(std::string_view(id), spec->key) ||
        !MatchesPrefabDefinition(marker, *spec)) {
      continue;
    }
    const svg::Reference link(RcString("#" + std::string(std::string_view(id))));
    const std::optional<svg::ResolvedReference> resolved =
        link.resolve(*marker.unsafeEntityHandle().registry());
    if (resolved.has_value() && resolved->handle.entity() == marker.unsafeEntityHandle().entity()) {
      return option.prefab;
    }
  }
  return std::nullopt;
}

}  // namespace donner::editor
