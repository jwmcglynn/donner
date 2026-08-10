#include "donner/editor/UiFrameGate.h"

namespace donner::editor {

UiFrameWork DecideUiFrameWork(const UiFrameGateInputs& inputs) {
  if (!inputs.editorWakePending && !inputs.browserInputPending && !inputs.idleTimerDue) {
    return UiFrameWork::Idle;
  }

  // A presentation-only frame republishes the worker document surface and nothing else. It is
  // only ever correct when the *sole* reason for this frame is the worker: a DOM event or a
  // scheduled animation wake both mean something in the UI itself is due to change.
  if (!inputs.editorWakePending || inputs.browserInputPending || inputs.idleTimerDue) {
    return UiFrameWork::FullUiFrame;
  }

  // Without an external surface holding the document, the document pixels live in the same
  // framebuffer the UI draws into, and skipping the pass would freeze the document too.
  if (!inputs.workerSurfacePresentsDocument || !inputs.documentPresentedByExternalSurface ||
      inputs.documentAbsent) {
    return UiFrameWork::FullUiFrame;
  }

  // Anything ImGui would animate, settle, or consume on its own next frame.
  if (inputs.imguiHasQueuedInputEvents || inputs.imguiItemActive || inputs.imguiPopupOpen ||
      inputs.imguiDragDropActive || inputs.imguiMovingWindow || inputs.imguiNavWindowing ||
      inputs.imguiWantsTextInput || inputs.imguiMouseButtonDown ||
      inputs.imguiHoverTimersPending) {
    return UiFrameWork::FullUiFrame;
  }

  // Shell obligations that only the full frame discharges, and shell state the UI displays that is
  // still moving.
  if (inputs.deferredRenderRequestPending || inputs.sidebarSnapshotRefreshPending ||
      inputs.deferredEditorWorkQueued || inputs.samplePickerVisible ||
      inputs.contentOnlyCapturePending ||
      inputs.shellAnimationScheduled || inputs.lockedRejectionFlashActive ||
      inputs.toolGestureActive || inputs.pendingScrollEvents || inputs.viewportUninitialized ||
      inputs.windowGeometryChanged || inputs.reproRecording || inputs.diagnosticOverlayVisible) {
    return UiFrameWork::FullUiFrame;
  }

  return UiFrameWork::PresentationOnly;
}

}  // namespace donner::editor
