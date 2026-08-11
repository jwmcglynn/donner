#include "donner/editor/UiFrameGate.h"

#include <gtest/gtest.h>

#include <functional>
#include <string_view>
#include <vector>

namespace donner::editor {
namespace {

/// The one shape that is allowed to skip the ImGui pass: a worker epoch landed, the document is on
/// the external surface, and nothing in the UI is mid-anything. Every other test mutates exactly
/// one field of this and expects the skip to be withdrawn.
UiFrameGateInputs SettledWorkerFrame() {
  return UiFrameGateInputs{
      .editorWakePending = true,
      .browserInputPending = false,
      .idleTimerDue = false,
      .workerSurfacePresentsDocument = true,
      .documentPresentedByExternalSurface = true,
  };
}

TEST(UiFrameGateTest, NoWakeSourceIsIdle) {
  UiFrameGateInputs inputs = SettledWorkerFrame();
  inputs.editorWakePending = false;

  EXPECT_EQ(DecideUiFrameWork(inputs), UiFrameWork::Idle);
}

TEST(UiFrameGateTest, SettledWorkerWakeSkipsTheUiPass) {
  EXPECT_EQ(DecideUiFrameWork(SettledWorkerFrame()), UiFrameWork::PresentationOnly);
}

TEST(UiFrameGateTest, BrowserInputAlwaysRebuildsTheUi) {
  UiFrameGateInputs inputs = SettledWorkerFrame();
  inputs.browserInputPending = true;

  EXPECT_EQ(DecideUiFrameWork(inputs), UiFrameWork::FullUiFrame);
}

TEST(UiFrameGateTest, BrowserInputWithoutAWorkerWakeStillRebuildsTheUi) {
  UiFrameGateInputs inputs = SettledWorkerFrame();
  inputs.editorWakePending = false;
  inputs.browserInputPending = true;

  EXPECT_EQ(DecideUiFrameWork(inputs), UiFrameWork::FullUiFrame);
}

TEST(UiFrameGateTest, ScheduledAnimationWakeRebuildsTheUi) {
  UiFrameGateInputs inputs = SettledWorkerFrame();
  inputs.idleTimerDue = true;

  EXPECT_EQ(DecideUiFrameWork(inputs), UiFrameWork::FullUiFrame);
}

TEST(UiFrameGateTest, FramebufferUnderlayBuildsNeverSkipTheUiPass) {
  UiFrameGateInputs inputs = SettledWorkerFrame();
  inputs.workerSurfacePresentsDocument = false;
  inputs.documentPresentedByExternalSurface = false;

  EXPECT_EQ(DecideUiFrameWork(inputs), UiFrameWork::FullUiFrame);
}

TEST(UiFrameGateTest, ExternalSurfaceFallbackToTheUnderlayRebuildsTheUi) {
  UiFrameGateInputs inputs = SettledWorkerFrame();
  inputs.documentPresentedByExternalSurface = false;

  EXPECT_EQ(DecideUiFrameWork(inputs), UiFrameWork::FullUiFrame);
}

// Table-driven so that adding a field to UiFrameGateInputs without adding it here is visible: the
// count assertion below fails when the two drift apart.
struct BlockingFieldCase {
  std::string_view name;
  std::function<void(UiFrameGateInputs&)> set;
};

const std::vector<BlockingFieldCase>& BlockingFields() {
  static const std::vector<BlockingFieldCase>* kCases = new std::vector<BlockingFieldCase>{
      {"imguiHasQueuedInputEvents", [](UiFrameGateInputs& i) { i.imguiHasQueuedInputEvents = true; }},
      {"imguiItemActive", [](UiFrameGateInputs& i) { i.imguiItemActive = true; }},
      {"imguiPopupOpen", [](UiFrameGateInputs& i) { i.imguiPopupOpen = true; }},
      {"imguiDragDropActive", [](UiFrameGateInputs& i) { i.imguiDragDropActive = true; }},
      {"imguiMovingWindow", [](UiFrameGateInputs& i) { i.imguiMovingWindow = true; }},
      {"imguiNavWindowing", [](UiFrameGateInputs& i) { i.imguiNavWindowing = true; }},
      {"imguiWantsTextInput", [](UiFrameGateInputs& i) { i.imguiWantsTextInput = true; }},
      {"imguiMouseButtonDown", [](UiFrameGateInputs& i) { i.imguiMouseButtonDown = true; }},
      {"imguiHoverTimersPending", [](UiFrameGateInputs& i) { i.imguiHoverTimersPending = true; }},
      {"deferredRenderRequestPending",
       [](UiFrameGateInputs& i) { i.deferredRenderRequestPending = true; }},
      {"sidebarSnapshotRefreshPending",
       [](UiFrameGateInputs& i) { i.sidebarSnapshotRefreshPending = true; }},
      {"deferredEditorWorkQueued",
       [](UiFrameGateInputs& i) { i.deferredEditorWorkQueued = true; }},
      {"samplePickerVisible", [](UiFrameGateInputs& i) { i.samplePickerVisible = true; }},
      {"contentOnlyCapturePending",
       [](UiFrameGateInputs& i) { i.contentOnlyCapturePending = true; }},
      {"shellAnimationScheduled", [](UiFrameGateInputs& i) { i.shellAnimationScheduled = true; }},
      {"lockedRejectionFlashActive",
       [](UiFrameGateInputs& i) { i.lockedRejectionFlashActive = true; }},
      {"toolGestureActive", [](UiFrameGateInputs& i) { i.toolGestureActive = true; }},
      {"pendingScrollEvents", [](UiFrameGateInputs& i) { i.pendingScrollEvents = true; }},
      {"viewportUninitialized", [](UiFrameGateInputs& i) { i.viewportUninitialized = true; }},
      {"epochPlacementDeferred", [](UiFrameGateInputs& i) { i.epochPlacementDeferred = true; }},
      {"windowGeometryChanged", [](UiFrameGateInputs& i) { i.windowGeometryChanged = true; }},
      {"reproRecording", [](UiFrameGateInputs& i) { i.reproRecording = true; }},
      {"diagnosticOverlayVisible",
       [](UiFrameGateInputs& i) { i.diagnosticOverlayVisible = true; }},
      {"documentAbsent", [](UiFrameGateInputs& i) { i.documentAbsent = true; }},
  };
  return *kCases;
}

TEST(UiFrameGateTest, EveryModelledObligationWithdrawsTheSkip) {
  for (const BlockingFieldCase& blockingCase : BlockingFields()) {
    UiFrameGateInputs inputs = SettledWorkerFrame();
    blockingCase.set(inputs);

    EXPECT_EQ(DecideUiFrameWork(inputs), UiFrameWork::FullUiFrame)
        << "setting " << blockingCase.name << " must force a full UI frame";
  }
}

TEST(UiFrameGateTest, BlockingFieldTableCoversEveryBoolInTheInputStruct) {
  // UiFrameGateInputs is all bools. Three are wake sources and two describe the presentation
  // target; those have their own dedicated tests above because they are not simple "true blocks
  // the skip" fields. Everything else must appear in the table.
  constexpr std::size_t kWakeSourceFields = 3;
  constexpr std::size_t kPresentationTargetFields = 2;
  const std::size_t totalBoolFields = sizeof(UiFrameGateInputs) / sizeof(bool);

  EXPECT_EQ(BlockingFields().size(),
            totalBoolFields - kWakeSourceFields - kPresentationTargetFields)
      << "a field was added to UiFrameGateInputs without a case in BlockingFields()";
}

TEST(UiFrameGateTest, HoverSettleWindowClearsImGuiLongestBuiltInDelay) {
  // ImGui's HoverDelayNormal is 0.40s and HoverStationaryDelay is 0.15s. Skipped frames make the
  // frame delta jump, so the settle window has to sit strictly above the longest of those.
  EXPECT_GT(kUiFrameHoverSettleSeconds, 0.40f);
}

}  // namespace
}  // namespace donner::editor
