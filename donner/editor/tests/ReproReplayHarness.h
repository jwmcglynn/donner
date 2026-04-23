#pragma once
/// @file
///
/// Test-only harness for replaying a `.donner-repro` recording through
/// the real editor stack — `EditorApp` + `SelectTool` + `RenderCoordinator`
/// + `AsyncRenderer` — so tests can assert on both behavior (which
/// element was selected, DOM mutations applied, frame timing) and the
/// rendered bitmap at arbitrary checkpoints.
///
/// This is scoped tightly to the tests in `donner/editor/tests/` that
/// already live against `donner_splash.svg`. It deliberately does NOT
/// try to be the general `ReproPlayer` that
/// `docs/design_docs/0029-ui_input_repro.md` describes — when that
/// lands this harness collapses into it.
///
/// Replay is driven by the v2 recording's authoritative
/// `frame.mouseDocX` / `mouseDocY` coords. No screen→doc reconstruction
/// from pane-layout constants — the recording carries the live viewport
/// it was captured under, and the harness feeds that straight into
/// `ViewportState`.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/editor/backend_lib/AsyncRenderer.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

class EditorApp;
class RenderCoordinator;
class SelectTool;

/// Per-frame timing captured by the replay so tests can assert on
/// worker latency without reaching through `AsyncRenderer` themselves.
struct ReplayFrameTiming {
  std::uint64_t reproFrameIndex = 0;
  double flushFrameMs = 0.0;
  double workerMs = 0.0;
  double totalFrameMs = 0.0;
  bool wasBusy = false;
};

/// Optional per-replay configuration. All fields opt-in — callers set
/// what they need and leave the rest default.
struct ReplayConfig {
  /// When `true`, the replay enables `SelectTool::setCompositedDragPreviewEnabled`
  /// regardless of what the recording metadata says. Use for the
  /// composited-promote test (which always wants the composited drag
  /// path on, even if the recording was captured without
  /// `--experimental`).
  bool forceCompositedDragPreview = false;

  /// Frame indices at which the harness should invoke
  /// `onCheckpoint` after the frame's render lands. Indices match the
  /// `ReproFrame::index` values in the recording. Out-of-order or
  /// out-of-range indices are silently ignored.
  std::vector<std::uint64_t> checkpointFrames;

  /// Invoked at each checkpoint frame, in replay order, with:
  ///   - the render result from that frame (may be `nullopt` if the
  ///     renderer was busy), and
  ///   - the checkpoint's 0-based index into `checkpointFrames`.
  /// Called from the replay loop inside `ReplayRepro`; the
  /// `EditorApp` / `SVGDocument` handles referenced by the result
  /// are still live for the duration of the call.
  std::function<void(std::size_t checkpointIndex, const RenderResult* result)>
      onCheckpoint;

  /// When `true`, after the replay's last frame the harness clears
  /// the selection and emits one final render request. That matches
  /// the live editor's post-drag "settle render" — demoting the
  /// promoted entity lets the compositor drop its split-layer cache
  /// and refresh the flat bitmap with the post-drag DOM state.
  ///
  /// Without this, a recording that ends with an active selection
  /// leaves the flat bitmap frozen at whatever it was before the
  /// first drag began (the compositor keeps the entity promoted on
  /// selection hold and skips the main compose during split). Tests
  /// that want to pixel-diff the "what the user sees after all
  /// drags complete" flat bitmap need the settle render. The result
  /// is stored in `ReplayResults::settleRender` (not `finalRender`,
  /// which stays the last replay-frame render).
  bool appendSettleFrame = false;
};

/// Result of `ReplayRepro`. Collected while the document is still live.
struct ReplayResults {
  std::vector<ReplayFrameTiming> frames;
  std::vector<std::uint64_t> mouseDownFrameIndices;
  std::vector<std::uint64_t> mouseUpFrameIndices;

  /// First successful render result captured before any mouse-down
  /// event fired. Represents the editor's cold rendering of the
  /// document — ground truth for "what the user expects to see".
  std::optional<RenderResult> preDragRender;
  /// Last successful render result. Represents the post-release
  /// bitmap the user would see after all gestures have resolved.
  std::optional<RenderResult> finalRender;
  /// Result of the post-replay settle render (only populated when
  /// `ReplayConfig::appendSettleFrame` is true). The flat bitmap
  /// here reflects the final DOM state with no promoted entity —
  /// the right thing to pixel-diff for "post-drag looks correct".
  std::optional<RenderResult> settleRender;

  /// `canvasFromDocumentTransform()` at the end of the last frame.
  /// Callers can map document-space bounds into bitmap-pixel
  /// coordinates for content checks.
  Transform2d canvasFromDocumentAtEnd;

  /// Per-mouse-up selection tag + id pair, in gesture order. Empty
  /// strings when nothing was selected at mouse-up time.
  std::vector<std::string> selectionElementTags;
  std::vector<std::string> selectionElementIds;
  /// Renderable-geometry world-bounds union for each mouse-up's
  /// selection, in gesture order. `std::nullopt` when the selection
  /// has no visible geometry (e.g. an empty group). Captured inline
  /// while the `SVGDocument` is still live so the boxes stay valid
  /// after `ReplayRepro` returns.
  std::vector<std::optional<Box2d>> selectionWorldBoundsAtMouseUp;

  /// Fast-path counters reported by the async renderer — useful for
  /// tests that care about whether the composited fast path stayed
  /// engaged across drag frames.
  std::uint64_t fastPathFrames = 0;
  std::uint64_t slowPathFramesWithDirty = 0;
  std::uint64_t noDirtyFrames = 0;
};

/// Replay the given recording against a fresh `EditorApp` loaded with
/// `svgPath`. Invokes `config.onCheckpoint` at each requested frame.
/// Returns the aggregated results; failures produce `ASSERT_*` on the
/// current gtest test.
ReplayResults ReplayRepro(const std::filesystem::path& reproPath,
                          const std::filesystem::path& svgPath, const ReplayConfig& config = {});

}  // namespace donner::editor
