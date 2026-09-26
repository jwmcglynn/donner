#pragma once
/// @file
/// Reusable stroke markers inserted into the current SVG only when chosen.

#include <array>
#include <optional>
#include <string_view>

namespace donner::svg {
class SVGDocument;
class SVGElement;
}  // namespace donner::svg

namespace donner::editor {

class EditorApp;

/// Marker shapes offered by the inspector before the document defines them.
enum class StrokeMarkerPrefab { FilledArrow, OpenArrow, Dot, Diamond };

/// Human-readable picker entry for one reusable marker shape.
struct StrokeMarkerPrefabOption {
  StrokeMarkerPrefab prefab;  ///< Shape selected by this entry.
  std::string_view label;     ///< Text shown in the marker picker.
};

/// Marker choices shown for both the start and end of a stroke.
inline constexpr std::array<StrokeMarkerPrefabOption, 4> kStrokeMarkerPrefabOptions = {{
    {StrokeMarkerPrefab::FilledArrow, "Filled arrow"},
    {StrokeMarkerPrefab::OpenArrow, "Open arrow"},
    {StrokeMarkerPrefab::Dot, "Dot"},
    {StrokeMarkerPrefab::Diamond, "Diamond"},
}};

/**
 * Attach a reusable marker to the selected shapes, creating its SVG definition on first use.
 *
 * The marker uses context-stroke so its color follows the shape. A later use of the same preset
 * reuses the existing tagged definition; an unrelated element with the preferred ID is preserved.
 * Definition insertion and the style change form one source-backed undo entry.
 *
 * @param app Editor containing the selected shapes and SVG document.
 * @param property Either `marker-start` or `marker-end`.
 * @param prefab Preset to use.
 * @return True when a document mutation was queued.
 */
bool ApplyStrokeMarkerPrefab(EditorApp& app, std::string_view property, StrokeMarkerPrefab prefab);

/// Identify a tagged prefab marker referenced by `url(#id)` in the current SVG.
std::optional<StrokeMarkerPrefab> StrokeMarkerPrefabForReference(svg::SVGDocument& document,
                                                                 std::string_view reference);

/// Identify a marker whose ID, resolved target, and SVG geometry match a built-in prefab.
std::optional<StrokeMarkerPrefab> StrokeMarkerPrefabForElement(const svg::SVGElement& marker);

}  // namespace donner::editor
