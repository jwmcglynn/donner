#pragma once
/// @file

#include <array>
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

/// Render one oriented rotate-cursor SVG to straight-alpha RGBA pixels.
[[nodiscard]] std::optional<RotateCursorImage> RenderRotateCursorImage(
    SelectionTransformCorner corner, std::shared_ptr<geode::GeodeDevice> geodeDevice);

/// RAII owner for the editor's custom rotate cursors.
class RotateCursorSet {
public:
  RotateCursorSet() = default;
  ~RotateCursorSet();

  RotateCursorSet(const RotateCursorSet&) = delete;
  RotateCursorSet& operator=(const RotateCursorSet&) = delete;
  RotateCursorSet(RotateCursorSet&&) = delete;
  RotateCursorSet& operator=(RotateCursorSet&&) = delete;

  /// Create all rotate cursors for @p window.
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

  /// Restore GLFW's default cursor if a custom rotate cursor is active.
  void clearIfActive();

  /// Whether all rotate cursors were created successfully.
  [[nodiscard]] bool valid() const { return valid_; }

private:
  void destroy();

  GLFWwindow* window_ = nullptr;
  std::array<GLFWcursor*, 4> rotateCursors_ = {};
  bool customCursorActive_ = false;
  bool valid_ = false;
};

}  // namespace donner::editor
