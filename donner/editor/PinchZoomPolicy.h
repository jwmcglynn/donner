#pragma once
/// @file
///
/// The single source of truth for the editor's zoom-step model.
///
/// Three independent producers feed the same render-pane classifier: the mouse
/// wheel, the native macOS pinch monitor, and the browser trackpad pinch
/// bridge. Each one has to be calibrated against the same step constant, so the
/// numbers live here and nowhere else. Duplicating them has already produced
/// one silent 2x drift between the desktop and browser pinch paths.

#include <cmath>

namespace donner::editor {

/// Multiplicative zoom step applied per +1.0 wheel/scroll unit.
///
/// `ClassifyRenderPaneScrollGesture` interprets a zoom gesture as
/// `pow(kWheelZoomStep, scrollDelta.y)`, and `PinchMagnificationToScrollDelta`
/// inverts that mapping. Every producer of scroll deltas is calibrated against
/// this one value.
inline constexpr double kWheelZoomStep = 1.1;

/// Wheel pixels that the Wasm GLFW port treats as one scroll unit.
///
/// emscripten-glfw converts a `DOM_DELTA_PIXEL` wheel event into
/// `yoffset = -deltaY / 100` (the same convention SDL uses). The divisor is
/// fixed by the port, so anything synthesizing wheel pixels for the editor has
/// to pre-multiply by it to land on the scroll units the classifier expects.
inline constexpr double kWasmWheelPixelsPerScrollUnit = 100.0;

/// Wheel `deltaY`, in CSS pixels, that the browser pinch bridge must synthesize
/// per unit of `ln(scale)` so a pinch to `scale` zooms the document by exactly
/// `scale`, matching the native macOS pinch path.
///
/// Derivation:
/// ```
///   bridge:      deltaY  = -K * ln(scale)
///   glfw port:   yoffset = -deltaY / kWasmWheelPixelsPerScrollUnit
///                        =  K * ln(scale) / kWasmWheelPixelsPerScrollUnit
///   classifier:  zoom    = pow(kWheelZoomStep, yoffset)
/// ```
/// Requiring `zoom == scale` gives
/// `K = kWasmWheelPixelsPerScrollUnit / ln(kWheelZoomStep) = 100 / ln(1.1)`,
/// which is approximately 1049.2059. That is the browser analogue of the
/// desktop identity
/// `pow(kWheelZoomStep, PinchMagnificationToScrollDelta(m, kWheelZoomStep)) == 1 + m`.
///
/// Engine coverage note: only WebKit reports the non-standard
/// `gesturestart`/`gesturechange` events the bridge listens to, so this
/// constant only affects Safari. Chromium and Firefox deliver trackpad pinch
/// as a raw `ctrl`+wheel event on the same channel as a real ctrl+mouse-wheel
/// zoom: Chromium synthesizes `deltaY = -100 * ln(1 + magnification)`
/// (components/input/touchpad_pinch_event_queue.cc) and Gecko synthesizes
/// `deltaY = -100 * magnification` (widget/InputData.cpp) - equal to first
/// order for real per-event magnifications. The two cases ARE separable: a
/// synthesized pinch carries the DOM ctrlKey flag while no Ctrl/Cmd key is
/// physically held, which the input bridge observes by comparing the
/// capture-phase DOM flag against `glfwGetKey`. Discriminated pinch events
/// get `kPinchScrollUnitGain` so a browser pinch matches the desktop's
/// `zoom = 1 + magnification` per gesture event; a real ctrl+mouse-wheel keeps
/// gain 1.0 and the discrete `kWheelZoomStep` per notch.
[[nodiscard]] inline double PinchWheelDeltaPerLnScale() {
  return kWasmWheelPixelsPerScrollUnit / std::log(kWheelZoomStep);
}

/// Gain applied to a discriminated browser trackpad-pinch scroll unit so the
/// classifier's `pow(kWheelZoomStep, units)` becomes `exp(rawUnits)`: with the
/// engines' `deltaY = -100 * ln(scale)` synthesis (100 px per scroll unit),
/// desktop parity requires multiplying units by `1 / ln(kWheelZoomStep)`.
/// This is the same derivation as the Safari bridge constant above
/// (`100 / ln(1.1) == 100 * kPinchScrollUnitGain`).
[[nodiscard]] inline double PinchScrollUnitGain() {
  return 1.0 / std::log(kWheelZoomStep);
}

/// Per-event clamp for discriminated pinch units, in post-gain scroll units:
/// caps one event's zoom factor at 1.5x (or 1/1.5) so a misclassified event is
/// merely brisk rather than catastrophic. Real trackpad pinch events carry
/// per-event magnifications well under this bound.
[[nodiscard]] inline double MaxPinchScrollUnitsPerEvent() {
  return std::log(1.5) / std::log(kWheelZoomStep);
}

}  // namespace donner::editor
