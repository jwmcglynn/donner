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
///
/// Browser pinch has exactly one gain authority. Producers - Chromium and Gecko
/// natively, WebKit through the bootstrap's `gesturechange` bridge - all emit
/// UNGAINED wheel deltas of `-kWasmWheelPixelsPerScrollUnit * ln(scale)`, and
/// `ApplyPinchScrollUnitGain` in the input bridge's pinch discriminator is the
/// only place that gain is applied. Gaining at both ends is a silent
/// `1 / ln(kWheelZoomStep)` speedup, which is what made Safari pinch-zoom
/// unusable; every engine now flows through identical policy code.

#include <algorithm>
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

/// Wheel `deltaY`, in CSS pixels, that a browser trackpad pinch carries per
/// unit of `ln(scale)`.
///
/// This is a wire-shape constant, not a gain. Chromium and Gecko already
/// deliver a trackpad pinch as a `ctrl`-flagged wheel event on the same channel
/// as a real ctrl+mouse-wheel zoom, using exactly this shape: Chromium
/// synthesizes `deltaY = -100 * ln(1 + magnification)`
/// (components/input/touchpad_pinch_event_queue.cc) and Gecko synthesizes
/// `deltaY = -100 * magnification` (widget/InputData.cpp), equal to first order
/// for real per-event magnifications. Only WebKit withholds that channel and
/// reports the non-standard `gesturestart`/`gesturechange` events instead, so
/// `donner/editor/wasm/editor-bootstrap.js` bridges those into the identical
/// ungained shape. The editor publishes this value to the page so the bridge
/// cannot drift from it.
///
/// ONE GAIN AUTHORITY. Every producer emits ungained units and the single
/// consumer-side gain, `ApplyPinchScrollUnitGain`, converts them once. The
/// Safari bridge used to pre-multiply by `1 / ln(kWheelZoomStep)` at synthesis
/// while the discriminator multiplied by it again at consumption, which zoomed
/// Safari about 10.5x too fast in log space until the per-event clamp pinned
/// every gesture to a full 1.5x step. Do not reintroduce a producer-side gain:
/// calibrate producers to this shape and leave the gain where it is.
[[nodiscard]] inline double PinchWheelDeltaPerLnScale() {
  return kWasmWheelPixelsPerScrollUnit;
}

/// Gain applied to a discriminated browser trackpad-pinch scroll unit so the
/// classifier's `pow(kWheelZoomStep, units)` becomes `exp(rawUnits)`: with the
/// engines' `deltaY = -100 * ln(scale)` synthesis (100 px per scroll unit),
/// desktop parity requires multiplying units by `1 / ln(kWheelZoomStep)`.
///
/// The two cases ARE separable: a synthesized pinch carries the DOM ctrlKey
/// flag while no Ctrl/Cmd key is physically held, which the input bridge
/// observes by comparing the capture-phase DOM flag against `glfwGetKey`.
/// Discriminated pinch events get this gain so a browser pinch matches the
/// desktop's `zoom = 1 + magnification` per gesture event; a real
/// ctrl+mouse-wheel keeps gain 1.0 and the discrete `kWheelZoomStep` per notch.
///
/// Prefer `ApplyPinchScrollUnitGain`, which also applies the safety clamp.
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

/// Apply the one and only pinch gain to a raw scroll-unit delta.
///
/// This is the single consumption-side gain authority for every browser pinch
/// path. `EditorInputBridge` calls it once per discriminated pinch event and
/// nothing else in the editor may multiply pinch units again; producers hand
/// over ungained units and this function turns them into the scroll units the
/// classifier's `pow(kWheelZoomStep, units)` expects.
///
/// @param rawScrollUnits Ungained scroll units, i.e. `-deltaY / 100` straight
///   out of the Wasm GLFW port.
/// @return Gained units, clamped to `MaxPinchScrollUnitsPerEvent()`.
[[nodiscard]] inline double ApplyPinchScrollUnitGain(double rawScrollUnits) {
  const double maxUnits = MaxPinchScrollUnitsPerEvent();
  return std::clamp(rawScrollUnits * PinchScrollUnitGain(), -maxUnits, maxUnits);
}

}  // namespace donner::editor
