#pragma once
/// @file
///
/// Layer-lock state shared between the Layers panel (which draws the lock
/// affordance and toggles it) and `EditorApp`'s edit-gating path (which drops
/// geometry-changing and destructive mutations targeting a locked element).
///
/// A layer is "locked" when it — or any ancestor — carries the non-standard
/// `data-donner-locked="true"` marker attribute. Using a `data-*` attribute
/// keeps the marker round-tripping through SVG serialization without colliding
/// with any presentation attribute or affecting rendering.
///
/// This lives in its own translation unit (rather than in `LayerTreeModel` or
/// `EditorApp`) because both of those targets need `IsLocked` but
/// `layer_tree_model` already depends on `editor_app`; a shared low-level
/// target avoids a dependency cycle.

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

}  // namespace donner::editor
