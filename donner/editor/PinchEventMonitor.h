#pragma once
/// @file
///
/// Platform-specific render-pane pinch gesture capture. On macOS this
/// installs a local magnify-event monitor and feeds synthetic zoom
/// scroll events into the existing editor queue.

#include <vector>

#include "donner/editor/backend_lib/RenderPaneGesture.h"

struct GLFWwindow;

namespace donner::editor {

/// Convert an NSEvent magnification delta into the equivalent vertical
/// scroll delta for the existing wheel-zoom model.
///
/// The classifier interprets zoom as `pow(wheelZoomStep, scrollDeltaY)`.
/// Pinch magnification instead reports the multiplicative zoom delta as
/// `1 + magnification`, so this helper computes the inverse mapping.
///
/// @param magnification Cocoa magnification delta (e.g. 0.2 for 20%
///   pinch-out).
/// @param wheelZoomStep Multiplicative zoom step per +1.0 scroll unit.
/// @return Synthetic `scrollDelta.y` that yields the same zoom factor,
///   or 0.0 for degenerate inputs.
[[nodiscard]] double PinchMagnificationToScrollDelta(double magnification, double wheelZoomStep);

/// Install the native pinch monitor for the editor window.
///
/// On macOS this registers an `NSEventMaskMagnify` local monitor that
/// pushes synthetic zoom-marked scroll events into `events`. Other
/// platforms provide a stub that returns `false`.
///
/// @param window GLFW window that owns the render pane.
/// @param events Scroll-event queue drained by the render pane each
///   frame.
/// @param wheelZoomStep Multiplicative zoom step per +1.0 scroll unit.
/// @return True if the native monitor was installed or was already
///   active for this process.
[[nodiscard]] bool InstallPinchEventMonitor(GLFWwindow* window,
                                            std::vector<RenderPaneScrollEvent>* events,
                                            double wheelZoomStep);

}  // namespace donner::editor
