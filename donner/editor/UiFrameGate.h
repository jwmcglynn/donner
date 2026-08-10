#pragma once
/// @file
/// Decides, once per browser animation frame, whether the editor must rebuild its whole
/// immediate-mode UI or can present the worker-owned document surface on its own.
///
/// The browser build already runs a demand-driven main loop: a frame happens only when the shell,
/// the render worker, a DOM input event, or a scheduled animation wake asks for one. That gate is
/// binary, so a render worker publishing a fresh document epoch costs the same as a keystroke -
/// the full ImGui pass reruns even though the only thing that changed lives on a different canvas.
/// During a pinch/zoom gesture the worker publishes roughly one epoch per gesture burst, so about
/// half of all frames pay for a UI rebuild that produces byte-identical draw data.
///
/// This gate adds a third outcome between "sleep" and "rebuild everything": present the document
/// surface and skip the ImGui pass entirely. It is deliberately a pure function over an explicit
/// input struct so the whole idleness predicate is one readable expression, is unit-testable
/// headlessly, and cannot silently grow implicit dependencies on shell internals.
///
/// The predicate is conservative by construction: every field below is a reason to rebuild, and
/// the skip requires all of them to be quiet. Anything not modelled here therefore defaults to a
/// rebuild only if its absence keeps some other field set - so when adding editor state that the
/// UI displays, add a field here too rather than assuming the existing ones cover it.

#include <cstdint>

namespace donner::editor {

/// What one animation frame of the browser main loop has to do.
enum class UiFrameWork : std::uint8_t {
  /// Nothing is pending. Return without touching the editor.
  Idle,
  /// Republish the worker-owned document surface and skip the ImGui pass. The UI canvas keeps
  /// showing the last frame it presented, which is unchanged by construction.
  PresentationOnly,
  /// Run the ordinary frame: ImGui NewFrame, the whole widget pass, and a UI canvas present.
  FullUiFrame,
};

/**
 * Everything the frame gate is allowed to look at.
 *
 * Grouped by where the value is sampled. All ImGui fields are read *between* frames (after the
 * previous `ImGui::Render`, before the next `ImGui::NewFrame`), which is the only point where the
 * context describes a settled UI rather than one under construction.
 */
struct UiFrameGateInputs {
  // ---------------------------------------------------------------------------------------------
  // Wake sources for this animation frame.
  // ---------------------------------------------------------------------------------------------

  /// An in-process `wakeEventLoop()` is outstanding: the render worker finished an epoch, or shell
  /// code asked for a follow-up frame.
  bool editorWakePending = false;
  /// A DOM event the page listens for (pointer, wheel, key, composition, paste, resize, focus,
  /// blur, visibilitychange) arrived since the last frame. Any of these can change what the UI
  /// draws, so they always force a rebuild.
  bool browserInputPending = false;
  /// A scheduled animation wake came due (caret blink, text flash, rope animation, sync retry,
  /// surface retry). These exist precisely because something must be redrawn on a timer.
  bool idleTimerDue = false;

  // ---------------------------------------------------------------------------------------------
  // Build/platform capability.
  // ---------------------------------------------------------------------------------------------

  /// True only on builds where document pixels live on a surface outside the window framebuffer.
  /// On the framebuffer-underlay builds (desktop) the document is composited *into* the same
  /// surface the UI draws to, so skipping the UI pass would also freeze the document.
  bool workerSurfacePresentsDocument = false;
  /// The previous frame actually resolved to that external surface. A browser frame that fell back
  /// to the framebuffer underlay (no accepted worker epoch yet, a document swap in flight) is in
  /// the desktop situation above and must keep drawing.
  bool documentPresentedByExternalSurface = false;

  // ---------------------------------------------------------------------------------------------
  // ImGui context state, sampled between frames.
  // ---------------------------------------------------------------------------------------------

  /// ImGui still holds trickled input events (it deliberately spreads a same-frame press+release
  /// over two frames). Those frames change widget state with no new DOM event behind them.
  bool imguiHasQueuedInputEvents = false;
  /// A widget owns the interaction: a button held down, a slider or splitter being dragged, an
  /// open text field. Active widgets animate and consume input every frame.
  bool imguiItemActive = false;
  /// Any popup, modal, menu, or combo is open. Popups position and fade themselves per frame.
  bool imguiPopupOpen = false;
  /// A drag-and-drop payload is in flight; its preview follows the cursor.
  bool imguiDragDropActive = false;
  /// A window is being moved or resized by the user.
  bool imguiMovingWindow = false;
  /// The Ctrl+Tab window switcher overlay is up.
  bool imguiNavWindowing = false;
  /// Keyboard focus is inside a text field, so the caret must keep blinking.
  bool imguiWantsTextInput = false;
  /// A mouse button is still held. A press with no follow-up DOM event still has frame-driven
  /// consequences (repeat, hold-to-marquee, drag thresholds).
  bool imguiMouseButtonDown = false;
  /// A hover-driven timer can still change what is drawn: the hovered item changed since last
  /// frame, or a tooltip/stationary delay has not yet saturated. Skipped frames make time jump,
  /// so a delay that has *not* already fired must never be crossed while the UI is frozen.
  bool imguiHoverTimersPending = false;

  // ---------------------------------------------------------------------------------------------
  // Editor shell state.
  // ---------------------------------------------------------------------------------------------

  /// A render request is deferred to the end of the frame. It is submitted by the frame's tail,
  /// which a presentation-only frame does not run - skipping here would strand the request and
  /// leave the document one epoch behind the viewport for the rest of the gesture.
  bool deferredRenderRequestPending = false;
  /// The layers panel still owes thumbnail work. It renders one deferred thumbnail per UI pass and
  /// re-arms its own wake, so a skip would stall the carousel of thumbnails indefinitely.
  bool sidebarSnapshotRefreshPending = false;
  /// Editor work queued behind the async renderer's document lock: queued DOM mutations, history
  /// (undo/redo) actions, transform/source writebacks, a pending document replacement, or a loaded
  /// sample waiting to take the pane.
  ///
  /// This is the subtlest reason to rebuild. All of it is drained by the first frame that finds
  /// the renderer idle - and the frame that observes the renderer going idle is, by construction,
  /// the very worker-completion frame this gate wants to skip. Skipping it would strand the work
  /// until the next unrelated user input.
  bool deferredEditorWorkQueued = false;
  /// The welcome/sample picker owns the pane, so the document surface is suppressed and the UI is
  /// what the user is looking at.
  bool samplePickerVisible = false;
  /// A content-only capture is armed for this frame; captures must draw.
  bool contentOnlyCapturePending = false;
  /// The shell scheduled any animation wake at all (`nextIdleWakeSeconds`). Even when it is not
  /// due this frame, being mid-animation means the UI is not settled.
  bool shellAnimationScheduled = false;
  /// The locked-element rejection outline is fading; it animates on frame deltas.
  bool lockedRejectionFlashActive = false;
  /// A tool gesture is live: selection drag, marquee, pen drag, or an open text editing session.
  bool toolGestureActive = false;
  /// The input bridge still holds unconsumed scroll events. They are drained inside the render
  /// pane's widget pass.
  bool pendingScrollEvents = false;
  /// The viewport has not latched a pane size yet, so the pane geometry a presentation-only frame
  /// would reuse does not exist.
  bool viewportUninitialized = false;
  /// The window framebuffer size changed since the last full frame.
  bool windowGeometryChanged = false;
  /// A repro recording is capturing per-frame state; it must observe every frame.
  bool reproRecording = false;
  /// A diagnostic overlay that displays per-frame numbers is visible (perf HUD, compositor debug
  /// panel, tile overlay). Those readouts are only correct if every frame redraws them.
  bool diagnosticOverlayVisible = false;
  /// The editor has no document, so there is no worker surface worth presenting on its own.
  bool documentAbsent = false;
};

/**
 * Classify one animation frame.
 *
 * @param inputs Everything the decision is allowed to depend on.
 * @return `Idle` when no wake is outstanding, `PresentationOnly` when the frame can republish the
 *   worker document surface without rebuilding the UI, and `FullUiFrame` otherwise.
 */
[[nodiscard]] UiFrameWork DecideUiFrameWork(const UiFrameGateInputs& inputs);

/// Hover and tooltip delay timers are only safe to jump over once they have saturated. ImGui's
/// longest built-in delay is `HoverDelayNormal` (0.40s); this sits above it with margin so a
/// frame-time jump can never straddle a delay boundary that had not already fired.
inline constexpr float kUiFrameHoverSettleSeconds = 0.75f;

}  // namespace donner::editor
