#pragma once
/// @file
///
/// Layer-lock state shared between the Layers panel (which draws the lock
/// affordance and toggles it) and `EditorApp`'s edit-gating path (which drops
/// geometry-changing and destructive mutations targeting a locked element).
///
/// A layer is "locked" when it - or any ancestor - carries the non-standard
/// `data-donner-locked="true"` marker attribute. Using a `data-*` attribute
/// keeps the marker round-tripping through SVG serialization without colliding
/// with any presentation attribute or affecting rendering.
///
/// This lives in its own translation unit (rather than in `LayerTreeModel` or
/// `EditorApp`) because both of those targets need `IsLocked` but
/// `layer_tree_model` already depends on `editor_app`; a shared low-level
/// target avoids a dependency cycle.

#include <optional>
#include <string_view>

#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Marker attribute name used to lock a layer against geometry-changing edits
/// and deletion.
inline constexpr std::string_view kLockedAttributeName = "data-donner-locked";

/// Attribute value that marks a layer locked. Any other value (including the
/// attribute being absent or set to `"false"`) reads as unlocked.
inline constexpr std::string_view kLockedAttributeValue = "true";

/// Whether @p element is locked: true when the element OR ANY ANCESTOR carries
/// `data-donner-locked="true"`. Locking a group therefore locks every
/// descendant. Shared by the Layers panel (to draw the lock affordance) and by
/// the edit-gating path in `EditorApp` (to reject transform/delete mutations on
/// locked elements).
[[nodiscard]] bool IsLocked(const svg::SVGElement& element);

/// The nearest ancestor-or-self of @p element that directly carries the
/// `data-donner-locked="true"` marker - i.e. the element that actually owns the
/// lock - or `std::nullopt` if neither @p element nor any ancestor is locked.
///
/// This walks the same ancestor chain as `IsLocked`, but returns the *locked
/// layer* rather than a boolean: clicking a `<rect>` inside a locked `<g>`
/// resolves to the `<g>`, so locked-rejection feedback (the red outline flash
/// and the Layers-panel row highlight) targets the whole locked layer rather
/// than just the clicked leaf. When @p element itself carries the marker, it is
/// returned unchanged.
[[nodiscard]] std::optional<svg::SVGElement> LockedAncestor(const svg::SVGElement& element);

}  // namespace donner::editor
