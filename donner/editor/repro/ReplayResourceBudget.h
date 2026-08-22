#pragma once
/// @file

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/repro/ReproFile.h"

namespace donner::editor::repro {

/** Estimated work performed by one semantic replay action. */
struct ReplaySemanticActionCost {
  std::size_t actions = 1;
  std::size_t selectionMutations = 0;
  std::size_t weightedWorkBytes = 0;
  bool valid = true;
};

/** Estimated source-mutation work from raw replay input dispatched for one frame. */
struct ReplayInputFrameCost {
  std::size_t inputMutations = 0;
  std::size_t selectionMutations = 0;
  std::size_t weightedWorkBytes = 0;
  bool valid = true;
};

inline constexpr std::size_t kMaximumReplaySelectionMutations = 8'192;

/**
 * Tracks repeat-capable mutation keys whose down state persists across replay frames.
 *
 * ImGui owns held-key state after a key-down edge and can synthesize a repeated editing shortcut on
 * a later frame even when that frame contains no input edge. Advance this state before estimating
 * each frame, then charge the returned source-rewrite units through the normal input budget before
 * dispatch. Modifier state comes from the recorded frame snapshot, matching ApplyInputOverride.
 */
class ReplayHeldMutationKeyState {
public:
  [[nodiscard]] std::size_t advanceFrame(const ReproFrame& frame) {
    std::array<bool, kTrackedKeyCount> pressedThisFrame{};
    std::array<bool, kTrackedKeyCount> releasedThisFrame{};
    for (const ReproEvent& event : frame.events) {
      if (event.kind != ReproEvent::Kind::KeyDown && event.kind != ReproEvent::Kind::KeyUp) {
        continue;
      }
      const int index = trackedKeyIndex(event.key);
      if (index < 0) {
        continue;
      }
      if (event.kind == ReproEvent::Kind::KeyDown) {
        pressedThisFrame[static_cast<std::size_t>(index)] = true;
      } else {
        releasedThisFrame[static_cast<std::size_t>(index)] = true;
      }
    }

    std::size_t repeatedSourceRewrites = 0;
    for (std::size_t index = 0; index < kTrackedKeyCount; ++index) {
      // ApplyInputOverride queues all down edges before all up edges, so an up edge wins when both
      // are recorded in one frame.
      if (releasedThisFrame[index]) {
        held_[index] = false;
      } else if (pressedThisFrame[index]) {
        held_[index] = true;
      }
      if (held_[index] && !pressedThisFrame[index] &&
          heldKeyCanRewriteSource(index, frame.modifiers)) {
        ++repeatedSourceRewrites;
      }
    }
    return repeatedSourceRewrites;
  }

private:
  static constexpr std::size_t kTrackedKeyCount = 12;

  [[nodiscard]] static int trackedKeyIndex(int key) {
    if (key == ImGuiKey_Tab) return 0;
    if (key == ImGuiKey_Enter) return 1;
    if (key == ImGuiKey_KeypadEnter) return 2;
    if (key == ImGuiKey_Backspace) return 3;
    if (key == ImGuiKey_Delete) return 4;
    if (key == ImGuiKey_V) return 5;
    if (key == ImGuiKey_X) return 6;
    if (key == ImGuiKey_Z) return 7;
    if (key == ImGuiKey_Y) return 8;
    if (key == ImGuiKey_D) return 9;
    if (key == ImGuiKey_K) return 10;
    if (key == ImGuiKey_U) return 11;
    return -1;
  }

  [[nodiscard]] static bool heldKeyCanRewriteSource(std::size_t index, int modifiers) {
    constexpr int kCtrlModifier = 1 << 0;
    constexpr int kShiftModifier = 1 << 1;
    constexpr int kAltModifier = 1 << 2;
    constexpr int kSuperModifier = 1 << 3;
    const bool command = (modifiers & (kCtrlModifier | kSuperModifier)) != 0;
    const bool shift = (modifiers & kShiftModifier) != 0;
    const bool alt = (modifiers & kAltModifier) != 0;
    switch (index) {
      case 0: return !command && !shift && !alt;  // Tab / indent.
      case 1:
      case 2:
        // The source editor requires no modifiers, while in-canvas text accepts Alt and Shift.
        // Charge the union of both consumers.
        return !command;
      case 3:
      case 4:
        // In-canvas text repeats deletion regardless of modifiers; the source editor also maps
        // several modifier combinations to character, word, or selection deletion.
        return true;
      case 5:
      case 6:
      case 7:
      case 8:
      case 9: return command && !shift && !alt;  // Paste/cut/undo/redo/duplicate.
      case 10:
      case 11: return command && shift && !alt;  // Comment/uncomment.
      default: return false;
    }
  }

  std::array<bool, kTrackedKeyCount> held_{};
};

namespace detail {

[[nodiscard]] inline bool CheckedAdd(std::size_t left, std::size_t right, std::size_t* result) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

[[nodiscard]] inline bool CheckedMultiply(std::size_t left, std::size_t right,
                                          std::size_t* result) {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    return false;
  }
  *result = left * right;
  return true;
}

}  // namespace detail

/**
 * Estimate the mutation and source-rewrite work of one semantic replay action.
 *
 * A style change may inspect and rewrite the document source once per selected element. The
 * source, property, and value sizes are therefore charged for every affected element rather than
 * only once per action.
 */
[[nodiscard]] inline ReplaySemanticActionCost EstimateReplaySemanticActionCost(
    const ReproAction& action, std::size_t documentSourceBytes, std::size_t selectedElementCount) {
  constexpr std::size_t kBaseActionWorkBytes = 256;
  constexpr std::size_t kStyleMutationOverheadBytes = 512;

  ReplaySemanticActionCost cost{
      .weightedWorkBytes = kBaseActionWorkBytes,
  };
  if (action.kind != ReproAction::Kind::SetStyleProperty) {
    const std::size_t payloadBytes =
        action.kind == ReproAction::Kind::SetActiveTool ? action.tool.size() : 0;
    cost.valid =
        detail::CheckedAdd(cost.weightedWorkBytes, payloadBytes, &cost.weightedWorkBytes) &&
        detail::CheckedAdd(cost.weightedWorkBytes, documentSourceBytes, &cost.weightedWorkBytes) &&
        detail::CheckedAdd(cost.weightedWorkBytes, kStyleMutationOverheadBytes,
                           &cost.weightedWorkBytes);
    return cost;
  }

  cost.selectionMutations = selectedElementCount;
  std::size_t perMutationBytes = documentSourceBytes;
  if (!detail::CheckedAdd(perMutationBytes, action.propertyName.size(), &perMutationBytes) ||
      !detail::CheckedAdd(perMutationBytes, action.propertyValue.size(), &perMutationBytes) ||
      !detail::CheckedAdd(perMutationBytes, kStyleMutationOverheadBytes, &perMutationBytes)) {
    cost.valid = false;
    return cost;
  }

  // Even without a selection, changing the active paint style retains and parses the action
  // payload. Source-rewrite work is charged only for elements that will actually be mutated.
  std::size_t mutationBytes = 0;
  if (!detail::CheckedMultiply(perMutationBytes, selectedElementCount, &mutationBytes) ||
      !detail::CheckedAdd(cost.weightedWorkBytes, action.propertyName.size(),
                          &cost.weightedWorkBytes) ||
      !detail::CheckedAdd(cost.weightedWorkBytes, action.propertyValue.size(),
                          &cost.weightedWorkBytes) ||
      !detail::CheckedAdd(cost.weightedWorkBytes, mutationBytes, &cost.weightedWorkBytes)) {
    cost.valid = false;
  }
  return cost;
}

/** Conservatively estimate source mutation work reachable through raw frame input. */
[[nodiscard]] inline ReplayInputFrameCost EstimateReplayInputFrameCost(
    const ReproFrame& frame, std::size_t documentSourceBytes, std::size_t selectedElementCount,
    std::size_t potentialDocumentElementCount, std::size_t heldRepeatSourceRewriteUnits = 0) {
  constexpr std::size_t kInputMutationOverheadBytes = 2 * 1024;
  constexpr std::size_t kSelectionCandidateWorkBytes = 256;

  ReplayInputFrameCost cost{
      .inputMutations = heldRepeatSourceRewriteUnits,
  };
  std::size_t candidateScanUnits = 0;
  std::size_t shapeMutationUnits = 0;
  std::size_t singleSourceRewriteUnits = heldRepeatSourceRewriteUnits;
  bool selectionMayExpand = false;

  const auto addUnit = [&](std::size_t* units) {
    if (!detail::CheckedAdd(*units, 1, units)) {
      cost.valid = false;
    }
  };
  const auto addInputMutation = [&] { addUnit(&cost.inputMutations); };

  // A held pointer can hit-test the document and transform the live selection. It is consumed
  // after queued key/button transitions, so any same-frame selection expansion can affect it.
  if (frame.mouseButtonMask != 0) {
    addInputMutation();
    addUnit(&candidateScanUnits);
    addUnit(&shapeMutationUnits);
  }

  const auto isCommandModifier = [](int modifiers) {
    constexpr int kCtrlModifier = 1 << 0;
    constexpr int kSuperModifier = 1 << 3;
    return (modifiers & (kCtrlModifier | kSuperModifier)) != 0;
  };
  const auto isShiftModifier = [](int modifiers) {
    constexpr int kShiftModifier = 1 << 1;
    return (modifiers & kShiftModifier) != 0;
  };

  for (const ReproEvent& event : frame.events) {
    switch (event.kind) {
      case ReproEvent::Kind::MouseDown:
      case ReproEvent::Kind::MouseUp:
        addInputMutation();
        addUnit(&candidateScanUnits);
        // Button transitions may create or commit a pen/text edit even when no SVG selection is
        // active. Charge one source rewrite, independently of the candidate scan.
        addUnit(&singleSourceRewriteUnits);
        if (event.mouseButton == 0) {
          selectionMayExpand = true;
        }
        break;
      case ReproEvent::Kind::KeyDown: {
        addInputMutation();
        // The frame modifiers drive ImGui; event modifiers are retained for legacy recordings.
        // Treat either as active so inconsistent untrusted metadata cannot undercharge a shortcut.
        const int modifiers = frame.modifiers | event.modifiers;
        const bool command = isCommandModifier(modifiers);
        const bool shift = isShiftModifier(modifiers);

        if (command && !shift && event.key == ImGuiKey_A) {
          addUnit(&candidateScanUnits);
          selectionMayExpand = true;
          break;
        }

        const bool mutatesSelectedShapes =
            event.key == ImGuiKey_Delete || event.key == ImGuiKey_Backspace ||
            (command && (event.key == ImGuiKey_X || event.key == ImGuiKey_G ||
                         event.key == ImGuiKey_LeftBracket || event.key == ImGuiKey_RightBracket));
        if (mutatesSelectedShapes) {
          addUnit(&shapeMutationUnits);
          break;
        }

        // Text/source input and commit-capable tool shortcuts rewrite the document at most once;
        // they do not inspect or mutate every SVG candidate. Other keys pay fixed input overhead.
        const bool rewritesSourceOnce =
            event.key == ImGuiKey_Tab || event.key == ImGuiKey_Enter ||
            event.key == ImGuiKey_KeypadEnter || event.key == ImGuiKey_Escape ||
            (!command &&
             (event.key == ImGuiKey_V || event.key == ImGuiKey_P || event.key == ImGuiKey_T)) ||
            (command &&
             (event.key == ImGuiKey_V || event.key == ImGuiKey_F || event.key == ImGuiKey_Z ||
              event.key == ImGuiKey_Y || event.key == ImGuiKey_D || event.key == ImGuiKey_K ||
              event.key == ImGuiKey_B || event.key == ImGuiKey_I || event.key == ImGuiKey_U));
        if (rewritesSourceOnce) {
          addUnit(&singleSourceRewriteUnits);
        }
        break;
      }
      case ReproEvent::Kind::Char:
        addInputMutation();
        addUnit(&singleSourceRewriteUnits);
        break;
      case ReproEvent::Kind::KeyUp:
      case ReproEvent::Kind::Wheel:
      case ReproEvent::Kind::Resize:
      case ReproEvent::Kind::Focus: break;
    }
  }
  if (!cost.valid || cost.inputMutations == 0) {
    return cost;
  }

  // Registry component cardinality is available in O(1) and conservatively covers elements that a
  // raw gesture could scan, select, or mutate. Clamp one past the aggregate limit: exact counts
  // above the limit are immaterial because the reservation must reject them.
  const std::size_t boundedPotentialElementCount =
      std::min(potentialDocumentElementCount, kMaximumReplaySelectionMutations + 1);
  const std::size_t boundedSelectedElementCount =
      std::min(selectedElementCount, kMaximumReplaySelectionMutations + 1);
  const std::size_t candidateElements =
      std::max(boundedSelectedElementCount, boundedPotentialElementCount);
  const std::size_t shapeMutationFanout =
      selectionMayExpand ? candidateElements : boundedSelectedElementCount;

  std::size_t candidateScanMutations = 0;
  std::size_t shapeSelectionMutations = 0;
  if (!detail::CheckedMultiply(candidateScanUnits, candidateElements, &candidateScanMutations) ||
      !detail::CheckedMultiply(shapeMutationUnits, shapeMutationFanout, &shapeSelectionMutations) ||
      !detail::CheckedAdd(candidateScanMutations, shapeSelectionMutations,
                          &cost.selectionMutations)) {
    cost.valid = false;
    return cost;
  }

  // Candidate scans pay fixed work per possible element, but ordinary hit testing does not rewrite
  // the full source once per candidate. Shape mutations rewrite once per affected live selection;
  // only a same-frame expansion can raise that fanout to the potential document element count.
  // Source-editor character/paste work and tool commits are charged once, never per SVG candidate.
  const std::size_t shapeSourceRewriteFanout =
      shapeMutationUnits == 0 ? 0 : std::max(std::size_t{1}, shapeMutationFanout);
  std::size_t shapeSourceRewriteUnits = 0;
  std::size_t sourceRewriteUnits = 0;
  std::size_t sourceRewriteBytes = 0;
  std::size_t selectionWorkBytes = 0;
  std::size_t inputOverheadBytes = 0;
  if (!detail::CheckedMultiply(shapeMutationUnits, shapeSourceRewriteFanout,
                               &shapeSourceRewriteUnits) ||
      !detail::CheckedAdd(shapeSourceRewriteUnits, singleSourceRewriteUnits, &sourceRewriteUnits) ||
      !detail::CheckedMultiply(documentSourceBytes, sourceRewriteUnits, &sourceRewriteBytes) ||
      !detail::CheckedMultiply(cost.selectionMutations, kSelectionCandidateWorkBytes,
                               &selectionWorkBytes) ||
      !detail::CheckedMultiply(cost.inputMutations, kInputMutationOverheadBytes,
                               &inputOverheadBytes) ||
      !detail::CheckedAdd(sourceRewriteBytes, selectionWorkBytes, &cost.weightedWorkBytes) ||
      !detail::CheckedAdd(cost.weightedWorkBytes, inputOverheadBytes, &cost.weightedWorkBytes)) {
    cost.valid = false;
  }
  return cost;
}

/** Aggregate runtime budget for replay frames and semantic actions. */
class ReplayExecutionResourceBudget {
public:
  static constexpr std::size_t kMaximumPlaybackFrames = 1'024;
  static constexpr std::size_t kMaximumPixelFrames = 128 * 1024 * 1024;
  static constexpr std::size_t kMaximumActions = 4'096;
  static constexpr std::size_t kMaximumSelectionMutations = kMaximumReplaySelectionMutations;
  static constexpr std::size_t kMaximumInputMutations = 8'192;
  static constexpr std::size_t kMaximumWeightedWorkBytes = 128 * 1024 * 1024;

  [[nodiscard]] bool reserveFrame(std::size_t physicalPixels) {
    if (rejected_ || frames_ >= kMaximumPlaybackFrames ||
        physicalPixels > kMaximumPixelFrames - pixelFrames_) {
      rejected_ = true;
      return false;
    }
    ++frames_;
    pixelFrames_ += physicalPixels;
    return true;
  }

  [[nodiscard]] bool reserveAction(const ReplaySemanticActionCost& cost) {
    if (rejected_ || !cost.valid || cost.actions > kMaximumActions - actions_ ||
        cost.selectionMutations > kMaximumSelectionMutations - selectionMutations_ ||
        cost.weightedWorkBytes > kMaximumWeightedWorkBytes - weightedWorkBytes_) {
      rejected_ = true;
      return false;
    }
    actions_ += cost.actions;
    selectionMutations_ += cost.selectionMutations;
    weightedWorkBytes_ += cost.weightedWorkBytes;
    return true;
  }

  [[nodiscard]] bool reserveInput(const ReplayInputFrameCost& cost) {
    if (rejected_ || !cost.valid ||
        cost.inputMutations > kMaximumInputMutations - inputMutations_ ||
        cost.selectionMutations > kMaximumSelectionMutations - selectionMutations_ ||
        cost.weightedWorkBytes > kMaximumWeightedWorkBytes - weightedWorkBytes_) {
      rejected_ = true;
      return false;
    }
    inputMutations_ += cost.inputMutations;
    selectionMutations_ += cost.selectionMutations;
    weightedWorkBytes_ += cost.weightedWorkBytes;
    return true;
  }

  [[nodiscard]] std::size_t frames() const { return frames_; }
  [[nodiscard]] std::size_t pixelFrames() const { return pixelFrames_; }
  [[nodiscard]] std::size_t actions() const { return actions_; }
  [[nodiscard]] std::size_t selectionMutations() const { return selectionMutations_; }
  [[nodiscard]] std::size_t inputMutations() const { return inputMutations_; }
  [[nodiscard]] std::size_t weightedWorkBytes() const { return weightedWorkBytes_; }
  [[nodiscard]] bool rejected() const { return rejected_; }

private:
  std::size_t frames_ = 0;
  std::size_t pixelFrames_ = 0;
  std::size_t actions_ = 0;
  std::size_t selectionMutations_ = 0;
  std::size_t inputMutations_ = 0;
  std::size_t weightedWorkBytes_ = 0;
  bool rejected_ = false;
};

static_assert(ReplayExecutionResourceBudget::kMaximumPlaybackFrames == kMaximumReproPlaybackFrames);
static_assert(ReplayExecutionResourceBudget::kMaximumPixelFrames == kMaximumReproPixelFrames);

/**
 * Reserve semantic-action work before invoking the mutation callback.
 *
 * This helper is shared by production replay and structured fuzz oracles so the ordering guarantee
 * is directly exercised without requiring a window or graphics context.
 */
template <typename ApplyCallback>
[[nodiscard]] bool ApplyReplayActionWithResourceBudget(ReplayExecutionResourceBudget& budget,
                                                       const ReplaySemanticActionCost& cost,
                                                       ApplyCallback&& apply) {
  if (!budget.reserveAction(cost)) {
    return false;
  }
  std::forward<ApplyCallback>(apply)();
  return true;
}

/** Reserve raw-input mutation work before invoking the input dispatch callback. */
template <typename DispatchCallback>
[[nodiscard]] bool DispatchReplayInputWithResourceBudget(ReplayExecutionResourceBudget& budget,
                                                         const ReplayInputFrameCost& cost,
                                                         DispatchCallback&& dispatch) {
  if (!budget.reserveInput(cost)) {
    return false;
  }
  std::forward<DispatchCallback>(dispatch)();
  return true;
}

/** Result of applying one frame's actions and then dispatching its raw input. */
enum class ReplayFrameDispatchResult : std::uint8_t {
  Success,
  ActionBudgetExceeded,
  InputBudgetExceeded,
};

/**
 * Apply semantic actions before pricing and dispatching the raw input consumed later in the frame.
 *
 * The input estimator runs after every action callback, so it observes the resulting live document
 * size and selection. Every reservation also precedes its corresponding mutation callback.
 */
template <typename ActionRange, typename EstimateActionCallback, typename ApplyActionCallback,
          typename EstimateInputCallback, typename DispatchInputCallback>
[[nodiscard]] ReplayFrameDispatchResult DispatchReplayFrameWithResourceBudget(
    ReplayExecutionResourceBudget& budget, const ActionRange& actions,
    EstimateActionCallback&& estimateAction, ApplyActionCallback&& applyAction,
    EstimateInputCallback&& estimateInput, DispatchInputCallback&& dispatchInput) {
  for (const auto& action : actions) {
    const ReplaySemanticActionCost cost = estimateAction(action);
    if (!ApplyReplayActionWithResourceBudget(budget, cost, [&] { applyAction(action); })) {
      return ReplayFrameDispatchResult::ActionBudgetExceeded;
    }
  }

  const ReplayInputFrameCost inputCost = estimateInput();
  if (!DispatchReplayInputWithResourceBudget(budget, inputCost,
                                             std::forward<DispatchInputCallback>(dispatchInput))) {
    return ReplayFrameDispatchResult::InputBudgetExceeded;
  }
  return ReplayFrameDispatchResult::Success;
}

/** Aggregate retained-memory budget for replay diagnostics. */
class ReplayDiagnosticsResourceBudget {
public:
  static constexpr std::size_t kMaximumBytes = 16 * 1024 * 1024;
  static constexpr std::size_t kMaximumItems = 65'536;
  static constexpr std::size_t kMaximumStringBytes = 4 * 1024;
  static constexpr std::size_t kMaximumReadbackItemsPerVector = 4'096;
  static constexpr std::size_t kMaximumReadbackTextNodes = 4'096;

  [[nodiscard]] bool reserve(std::size_t bytes, std::size_t items) {
    if (rejected_ || bytes > kMaximumBytes - bytes_ || items > kMaximumItems - items_) {
      rejected_ = true;
      return false;
    }
    bytes_ += bytes;
    items_ += items;
    return true;
  }

  [[nodiscard]] std::size_t bytes() const { return bytes_; }
  [[nodiscard]] std::size_t items() const { return items_; }
  [[nodiscard]] bool rejected() const { return rejected_; }

private:
  std::size_t bytes_ = 0;
  std::size_t items_ = 0;
  bool rejected_ = false;
};

}  // namespace donner::editor::repro
