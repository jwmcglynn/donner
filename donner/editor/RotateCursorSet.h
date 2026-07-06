#pragma once
/// @file

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "donner/editor/SelectionTransformHandles.h"

struct GLFWcursor;
struct GLFWwindow;

namespace donner::geode {
class GeodeDevice;
}

namespace donner::editor {

/// RGBA pixels for one custom editor cursor image.
struct RotateCursorImage {
  /// Cursor width in pixels.
  int width = 0;

  /// Cursor height in pixels.
  int height = 0;

  /// Straight-alpha RGBA pixels, tightly packed.
  std::vector<unsigned char> rgba;
};

/// Visual state for the pan-mode cursor.
enum class PanCursorKind {
  /// Open hand while pan mode is armed.
  OpenHand,
  /// Closed hand while actively panning.
  ClosedHand,
};

/// Contextual pen-tool cursor variant, matching the pen tool's hover states.
enum class PenCursorHint {
  /// Plain nib: placing/continuing anchors.
  Base,
  /// Nib with a `+` badge: a click would insert an anchor on the hovered segment.
  Add,
  /// Nib with a `-` badge: a click would delete the hovered anchor.
  Remove,
  /// Nib with an `o` badge: a click would close the active contour.
  Close,
};

/// Every custom editor cursor, so the cursor set and its completeness /
/// snapshot tests share one enumeration. `Rotate` and `Scale` are oriented per
/// selection corner (see \ref CursorUsesCorner); the rest ignore the corner.
enum class EditorCursor : std::uint8_t {
  Select,
  Pen,
  PenAdd,
  PenRemove,
  PenClose,
  Rotate,
  Scale,
  PathModify,
  PanOpen,
  PanClosed,
};

/// Every editor cursor, in a stable order. Single source of truth for the
/// cursor-set completeness test.
inline constexpr std::array<EditorCursor, 10> kEditorCursors = {
    EditorCursor::Select,     EditorCursor::Pen,       EditorCursor::PenAdd,
    EditorCursor::PenRemove,  EditorCursor::PenClose,  EditorCursor::Rotate,
    EditorCursor::Scale,      EditorCursor::PathModify, EditorCursor::PanOpen,
    EditorCursor::PanClosed,
};

/// Pointer hotspot for a cursor, in 32x32 cursor-image pixels.
struct CursorHotspot {
  int x = 0;
  int y = 0;
};

/// Hotspot (pointer origin) for @p cursor, in cursor-image pixels. See
/// `donner/editor/art/STYLE.md` for the conventions.
[[nodiscard]] CursorHotspot HotspotForCursor(EditorCursor cursor);

/// Whether @p cursor's art is oriented per selection corner (rotate, scale).
[[nodiscard]] bool CursorUsesCorner(EditorCursor cursor);

/// Render any editor cursor to straight-alpha RGBA pixels. @p corner is only
/// consulted for corner-oriented cursors (\ref CursorUsesCorner); pass any
/// value otherwise. This is the one render path the completeness / snapshot
/// tests exercise, matching the pipeline the live cursors use.
[[nodiscard]] std::optional<RotateCursorImage> RenderEditorCursorImage(
    EditorCursor cursor, SelectionTransformCorner corner,
    std::shared_ptr<geode::GeodeDevice> geodeDevice);

/// Render one oriented rotate-cursor SVG to straight-alpha RGBA pixels.
[[nodiscard]] std::optional<RotateCursorImage> RenderRotateCursorImage(
    SelectionTransformCorner corner, std::shared_ptr<geode::GeodeDevice> geodeDevice);

/// Render one oriented scale-cursor SVG to straight-alpha RGBA pixels.
[[nodiscard]] std::optional<RotateCursorImage> RenderScaleCursorImage(
    SelectionTransformCorner corner, std::shared_ptr<geode::GeodeDevice> geodeDevice);

/// Render the select (arrow) cursor SVG to straight-alpha RGBA pixels.
[[nodiscard]] std::optional<RotateCursorImage> RenderSelectCursorImage(
    std::shared_ptr<geode::GeodeDevice> geodeDevice);

/// Render the path-modify (anchor-point) cursor SVG to straight-alpha RGBA pixels.
[[nodiscard]] std::optional<RotateCursorImage> RenderPathModifyCursorImage(
    std::shared_ptr<geode::GeodeDevice> geodeDevice);

/// Render the pan-mode cursor SVG for @p kind to straight-alpha RGBA pixels.
///
/// @param kind Pan cursor visual state to render.
/// @param geodeDevice Shared Geode device for Geode editor builds.
[[nodiscard]] std::optional<RotateCursorImage> RenderPanCursorImage(
    PanCursorKind kind, std::shared_ptr<geode::GeodeDevice> geodeDevice);

/// Render the base pen-tool cursor SVG to straight-alpha RGBA pixels.
///
/// @param geodeDevice Shared Geode device for Geode editor builds.
[[nodiscard]] std::optional<RotateCursorImage> RenderPenCursorImage(
    std::shared_ptr<geode::GeodeDevice> geodeDevice);

/// Render a contextual pen-tool cursor SVG for @p hint to straight-alpha RGBA pixels.
[[nodiscard]] std::optional<RotateCursorImage> RenderPenCursorImage(
    PenCursorHint hint, std::shared_ptr<geode::GeodeDevice> geodeDevice);

/// RAII owner for the editor's custom cursors.
class RotateCursorSet {
public:
  RotateCursorSet() = default;
  ~RotateCursorSet();

  RotateCursorSet(const RotateCursorSet&) = delete;
  RotateCursorSet& operator=(const RotateCursorSet&) = delete;
  RotateCursorSet(RotateCursorSet&&) = delete;
  RotateCursorSet& operator=(RotateCursorSet&&) = delete;

  /// Create all custom cursors for @p window.
  ///
  /// @param window GLFW window that will receive cursor changes.
  /// @param geodeDevice Shared Geode device for Geode editor builds.
  /// @return true if every cursor was created.
  [[nodiscard]] bool initialize(GLFWwindow* window,
                                std::shared_ptr<geode::GeodeDevice> geodeDevice);

  /// Set the custom rotate cursor matching @p corner.
  ///
  /// @param corner Selection corner under the pointer.
  /// @return true if a custom cursor was available and applied.
  [[nodiscard]] bool setRotateCursor(SelectionTransformCorner corner);

  /// Set the custom scale (resize) cursor matching @p corner.
  ///
  /// @param corner Selection corner under the pointer.
  /// @return true if a custom cursor was available and applied.
  [[nodiscard]] bool setScaleCursor(SelectionTransformCorner corner);

  /// Set the custom select (arrow) cursor.
  ///
  /// @return true if a custom cursor was available and applied.
  [[nodiscard]] bool setSelectCursor();

  /// Set the custom path-modify (anchor-point) cursor.
  ///
  /// @return true if a custom cursor was available and applied.
  [[nodiscard]] bool setPathModifyCursor();

  /// Set the custom pan cursor for @p kind.
  ///
  /// @param kind Pan cursor visual state to apply.
  /// @return true if a custom cursor was available and applied.
  [[nodiscard]] bool setPanCursor(PanCursorKind kind);

  /// Set the base pen-tool cursor.
  ///
  /// @return true if a custom cursor was available and applied.
  [[nodiscard]] bool setPenCursor();

  /// Set the contextual pen-tool cursor for @p hint.
  ///
  /// @param hint Pen cursor visual state (base/add/remove/close).
  /// @return true if a custom cursor was available and applied.
  [[nodiscard]] bool setPenCursor(PenCursorHint hint);

  /// Restore GLFW's default cursor if a custom cursor is active.
  void clearIfActive();

  /// Whether all custom cursors were created successfully.
  [[nodiscard]] bool valid() const { return valid_; }

private:
  void destroy();

  GLFWwindow* window_ = nullptr;
  std::array<GLFWcursor*, 4> rotateCursors_ = {};
  std::array<GLFWcursor*, 4> scaleCursors_ = {};
  std::array<GLFWcursor*, 2> panCursors_ = {};
  std::array<GLFWcursor*, 4> penCursors_ = {};  // Base/Add/Remove/Close.
  GLFWcursor* selectCursor_ = nullptr;
  GLFWcursor* pathModifyCursor_ = nullptr;
  bool customCursorActive_ = false;
  bool valid_ = false;
};

}  // namespace donner::editor
