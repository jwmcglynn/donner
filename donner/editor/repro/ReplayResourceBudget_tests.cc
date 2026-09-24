#include "donner/editor/repro/ReplayResourceBudget.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

namespace donner::editor::repro {
namespace {

using testing::ElementsAre;
using testing::Eq;

constexpr int kCtrl = 1 << 0;
constexpr int kShift = 1 << 1;
constexpr int kAlt = 1 << 2;
constexpr int kSuper = 1 << 3;

struct SemanticCostState {
  std::size_t actions;
  std::size_t selectionMutations;
  std::size_t weightedWorkBytes;
  bool valid;
  bool operator==(const SemanticCostState&) const = default;
};

void PrintTo(const SemanticCostState& state, std::ostream* os) {
  *os << "{actions=" << state.actions << ", selectionMutations=" << state.selectionMutations
      << ", weightedWorkBytes=" << state.weightedWorkBytes << ", valid=" << state.valid << '}';
}

SemanticCostState StateOf(const ReplaySemanticActionCost& cost) {
  return {cost.actions, cost.selectionMutations, cost.weightedWorkBytes, cost.valid};
}

struct InputCostState {
  std::size_t inputMutations;
  std::size_t selectionMutations;
  std::size_t weightedWorkBytes;
  bool valid;
  bool operator==(const InputCostState&) const = default;
};

void PrintTo(const InputCostState& state, std::ostream* os) {
  *os << "{inputMutations=" << state.inputMutations
      << ", selectionMutations=" << state.selectionMutations
      << ", weightedWorkBytes=" << state.weightedWorkBytes << ", valid=" << state.valid << '}';
}

InputCostState StateOf(const ReplayInputFrameCost& cost) {
  return {cost.inputMutations, cost.selectionMutations, cost.weightedWorkBytes, cost.valid};
}

struct BudgetState {
  std::size_t frames;
  std::size_t pixelFrames;
  std::size_t actions;
  std::size_t selectionMutations;
  std::size_t inputMutations;
  std::size_t weightedWorkBytes;
  bool rejected;
  bool operator==(const BudgetState&) const = default;
};

void PrintTo(const BudgetState& state, std::ostream* os) {
  *os << "{frames=" << state.frames << ", pixelFrames=" << state.pixelFrames
      << ", actions=" << state.actions << ", selectionMutations=" << state.selectionMutations
      << ", inputMutations=" << state.inputMutations
      << ", weightedWorkBytes=" << state.weightedWorkBytes << ", rejected=" << state.rejected
      << '}';
}

BudgetState StateOf(const ReplayExecutionResourceBudget& budget) {
  return {budget.frames(),         budget.pixelFrames(),
          budget.actions(),        budget.selectionMutations(),
          budget.inputMutations(), budget.weightedWorkBytes(),
          budget.rejected()};
}

ReproEvent KeyEvent(ReproEvent::Kind kind, int key, int modifiers = 0) {
  return ReproEvent{.kind = kind, .key = key, .modifiers = modifiers};
}

TEST(ReplayResourceBudget, HeldMutationKeysChargeOnlyUnderTheirRecordedFrameModifiers) {
  struct Case {
    const char* name;
    int key;
    int modifiers;
    std::size_t expectedRewrites;
  };
  const std::vector<Case> cases = {
      {"bare Tab", ImGuiKey_Tab, 0, 1},
      {"modified Tab", ImGuiKey_Tab, kCtrl, 0},
      {"bare Enter", ImGuiKey_Enter, 0, 1},
      {"Alt Enter", ImGuiKey_Enter, kAlt, 1},
      {"Ctrl Enter", ImGuiKey_Enter, kCtrl, 0},
      {"keypad Enter", ImGuiKey_KeypadEnter, kShift, 1},
      {"Super keypad Enter", ImGuiKey_KeypadEnter, kSuper, 0},
      {"Backspace with modifiers", ImGuiKey_Backspace, kCtrl | kShift, 1},
      {"Delete with modifiers", ImGuiKey_Delete, kAlt | kSuper, 1},
      {"command V", ImGuiKey_V, kCtrl, 1},
      {"command X", ImGuiKey_X, kSuper, 1},
      {"command Z", ImGuiKey_Z, kCtrl, 1},
      {"command Y", ImGuiKey_Y, kSuper, 1},
      {"command D", ImGuiKey_D, kCtrl, 1},
      {"shifted command V", ImGuiKey_V, kCtrl | kShift, 0},
      {"alt command X", ImGuiKey_X, kCtrl | kAlt, 0},
      {"command K with Shift", ImGuiKey_K, kCtrl | kShift, 1},
      {"command U with Shift", ImGuiKey_U, kSuper | kShift, 1},
      {"command K without Shift", ImGuiKey_K, kCtrl, 0},
      {"command U with Alt", ImGuiKey_U, kCtrl | kShift | kAlt, 0},
      {"untracked A", ImGuiKey_A, kCtrl, 0},
  };

  for (const Case& entry : cases) {
    SCOPED_TRACE(entry.name);
    ReplayHeldMutationKeyState held;
    ReproFrame pressed;
    pressed.modifiers = entry.modifiers;
    pressed.events = {KeyEvent(ReproEvent::Kind::KeyDown, entry.key)};
    EXPECT_EQ(held.advanceFrame(pressed), 0u);

    ReproFrame repeated;
    repeated.modifiers = entry.modifiers;
    EXPECT_EQ(held.advanceFrame(repeated), entry.expectedRewrites);

    repeated.events = {KeyEvent(ReproEvent::Kind::KeyUp, entry.key)};
    EXPECT_EQ(held.advanceFrame(repeated), 0u);
    repeated.events.clear();
    EXPECT_EQ(held.advanceFrame(repeated), 0u);
  }
}

TEST(ReplayResourceBudget, KeyUpWinsOverDownAndFrameSnapshotControlsRepeats) {
  ReplayHeldMutationKeyState held;
  ReproFrame bothEdges;
  bothEdges.events = {KeyEvent(ReproEvent::Kind::KeyDown, ImGuiKey_Tab),
                      KeyEvent(ReproEvent::Kind::KeyUp, ImGuiKey_Tab)};
  EXPECT_EQ(held.advanceFrame(bothEdges), 0u);
  EXPECT_EQ(held.advanceFrame(ReproFrame{}), 0u);

  ReproFrame down;
  down.events = {KeyEvent(ReproEvent::Kind::KeyDown, ImGuiKey_V, kCtrl)};
  EXPECT_EQ(held.advanceFrame(down), 0u);
  EXPECT_EQ(held.advanceFrame(ReproFrame{}), 0u);
  ReproFrame commandFrame;
  commandFrame.modifiers = kCtrl;
  EXPECT_EQ(held.advanceFrame(commandFrame), 1u);
}

TEST(ReplayResourceBudget, SemanticAndPointerEstimatesChargeTheirWholeFanout) {
  const ReproAction style{
      .kind = ReproAction::Kind::SetStyleProperty, .propertyName = "fill", .propertyValue = "red"};
  EXPECT_THAT(StateOf(EstimateReplaySemanticActionCost(style, 100, 3)),
              Eq(SemanticCostState{1, 3, 2'120, true}));
  EXPECT_THAT(StateOf(EstimateReplaySemanticActionCost(style, 100, 0)),
              Eq(SemanticCostState{1, 0, 263, true}));

  const ReproAction tool{.kind = ReproAction::Kind::SetActiveTool, .tool = "pen"};
  EXPECT_THAT(StateOf(EstimateReplaySemanticActionCost(tool, 100, 3)),
              Eq(SemanticCostState{1, 0, 871, true}));

  ReproFrame heldPointer;
  heldPointer.mouseButtonMask = 1;
  EXPECT_THAT(StateOf(EstimateReplayInputFrameCost(heldPointer, 100, 2, 4)),
              Eq(InputCostState{1, 6, 3'784, true}));
}

TEST(ReplayResourceBudget, FramePixelActionAndInputLimitsAcceptExactBoundaryThenLatch) {
  ReplayExecutionResourceBudget frameBudget;
  for (std::size_t index = 0; index < ReplayExecutionResourceBudget::kMaximumPlaybackFrames;
       ++index) {
    SCOPED_TRACE(index);
    ASSERT_TRUE(frameBudget.reserveFrame(0));
  }
  EXPECT_THAT(StateOf(frameBudget), Eq(BudgetState{1'024, 0, 0, 0, 0, 0, false}));
  EXPECT_FALSE(frameBudget.reserveFrame(0));
  EXPECT_TRUE(frameBudget.rejected());

  ReplayExecutionResourceBudget pixelBudget;
  EXPECT_TRUE(pixelBudget.reserveFrame(ReplayExecutionResourceBudget::kMaximumPixelFrames));
  EXPECT_FALSE(pixelBudget.reserveFrame(1));
  EXPECT_THAT(
      StateOf(pixelBudget),
      Eq(BudgetState{1, ReplayExecutionResourceBudget::kMaximumPixelFrames, 0, 0, 0, 0, true}));

  ReplayExecutionResourceBudget actionBudget;
  EXPECT_TRUE(actionBudget.reserveAction(
      ReplaySemanticActionCost{.actions = ReplayExecutionResourceBudget::kMaximumActions}));
  EXPECT_FALSE(actionBudget.reserveAction(ReplaySemanticActionCost{}));
  EXPECT_THAT(StateOf(actionBudget), Eq(BudgetState{0, 0, 4'096, 0, 0, 0, true}));

  ReplayExecutionResourceBudget inputBudget;
  EXPECT_TRUE(inputBudget.reserveInput(ReplayInputFrameCost{
      .inputMutations = ReplayExecutionResourceBudget::kMaximumInputMutations}));
  EXPECT_FALSE(inputBudget.reserveInput(ReplayInputFrameCost{.inputMutations = 1}));
  EXPECT_THAT(StateOf(inputBudget), Eq(BudgetState{0, 0, 0, 0, 8'192, 0, true}));

  ReplayExecutionResourceBudget workBudget;
  EXPECT_TRUE(workBudget.reserveAction(ReplaySemanticActionCost{
      .weightedWorkBytes = ReplayExecutionResourceBudget::kMaximumWeightedWorkBytes}));
  EXPECT_FALSE(workBudget.reserveInput(ReplayInputFrameCost{.weightedWorkBytes = 1}));
  EXPECT_THAT(StateOf(workBudget),
              Eq(BudgetState{0, 0, 1, 0, 0,
                             ReplayExecutionResourceBudget::kMaximumWeightedWorkBytes, true}));
}

TEST(ReplayResourceBudget, SelectionFanoutIsReservedBeforeMutation) {
  ReproFrame heldPointer;
  heldPointer.mouseButtonMask = 1;
  const ReplayInputFrameCost exact =
      EstimateReplayInputFrameCost(heldPointer, 0, 1, kMaximumReplaySelectionMutations - 1);
  EXPECT_THAT(StateOf(exact),
              Eq(InputCostState{1, kMaximumReplaySelectionMutations,
                                kMaximumReplaySelectionMutations * 256 + 2'048, true}));

  ReplayExecutionResourceBudget budget;
  EXPECT_TRUE(budget.reserveInput(exact));
  const ReplayInputFrameCost over =
      EstimateReplayInputFrameCost(heldPointer, 0, 1, kMaximumReplaySelectionMutations);
  EXPECT_THAT(StateOf(over),
              Eq(InputCostState{1, kMaximumReplaySelectionMutations + 1,
                                (kMaximumReplaySelectionMutations + 1) * 256 + 2'048, true}));
  int dispatches = 0;
  ReplayExecutionResourceBudget freshBudget;
  EXPECT_FALSE(DispatchReplayInputWithResourceBudget(freshBudget, over, [&] { ++dispatches; }));
  EXPECT_THAT(StateOf(freshBudget), Eq(BudgetState{0, 0, 0, 0, 0, 0, true}));
  EXPECT_FALSE(DispatchReplayInputWithResourceBudget(budget, over, [&] { ++dispatches; }));
  EXPECT_EQ(dispatches, 0);
  EXPECT_THAT(StateOf(budget), Eq(BudgetState{0, 0, 0, kMaximumReplaySelectionMutations, 1,
                                              exact.weightedWorkBytes, true}));
}

TEST(ReplayResourceBudget, CheckedArithmeticAndEstimatesRejectOverflow) {
  constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
  std::size_t result = 17;
  EXPECT_FALSE(detail::CheckedAdd(kMax, 1, &result));
  EXPECT_EQ(result, 17u);
  EXPECT_FALSE(detail::CheckedMultiply(kMax, 2, &result));
  EXPECT_EQ(result, 17u);

  const ReproAction style{
      .kind = ReproAction::Kind::SetStyleProperty, .propertyName = "fill", .propertyValue = "red"};
  const ReplaySemanticActionCost semantic = EstimateReplaySemanticActionCost(style, kMax, 2);
  EXPECT_FALSE(semantic.valid);
  ReproFrame heldPointer;
  heldPointer.mouseButtonMask = 1;
  const ReplayInputFrameCost input = EstimateReplayInputFrameCost(heldPointer, kMax, 1, 1);
  EXPECT_FALSE(input.valid);

  ReplayExecutionResourceBudget budget;
  EXPECT_FALSE(budget.reserveAction(semantic));
  EXPECT_FALSE(budget.reserveInput(ReplayInputFrameCost{}));
  EXPECT_THAT(StateOf(budget), Eq(BudgetState{0, 0, 0, 0, 0, 0, true}));

  ReplayExecutionResourceBudget inputBudget;
  EXPECT_FALSE(inputBudget.reserveInput(input));
  EXPECT_FALSE(inputBudget.reserveAction(ReplaySemanticActionCost{}));
  EXPECT_TRUE(inputBudget.rejected());
}

TEST(ReplayResourceBudget, FrameDispatchPricesInputAfterActionsAndReservesBeforeCallbacks) {
  ReplayExecutionResourceBudget budget;
  std::size_t sourceBytes = 10;
  std::size_t selection = 1;
  std::vector<std::string> order;
  const std::vector<int> actions{1, 2};
  const ReplayFrameDispatchResult result = DispatchReplayFrameWithResourceBudget(
      budget, actions,
      [&](int action) {
        order.push_back("estimate " + std::to_string(action));
        return ReplaySemanticActionCost{.weightedWorkBytes = 10};
      },
      [&](int action) {
        EXPECT_EQ(budget.actions(), static_cast<std::size_t>(action));
        order.push_back("apply " + std::to_string(action));
        sourceBytes += static_cast<std::size_t>(action);
        ++selection;
      },
      [&] {
        order.push_back("estimate input");
        EXPECT_EQ(sourceBytes, 13u);
        EXPECT_EQ(selection, 3u);
        return ReplayInputFrameCost{
            .inputMutations = 1, .selectionMutations = selection, .weightedWorkBytes = sourceBytes};
      },
      [&] {
        EXPECT_EQ(budget.inputMutations(), 1u);
        order.push_back("dispatch input");
      });

  EXPECT_EQ(result, ReplayFrameDispatchResult::Success);
  EXPECT_THAT(order, ElementsAre("estimate 1", "apply 1", "estimate 2", "apply 2", "estimate input",
                                 "dispatch input"));
  EXPECT_THAT(StateOf(budget), Eq(BudgetState{0, 0, 2, 3, 1, 33, false}));
}

TEST(ReplayResourceBudget, RefusedFrameStepsNeverInvokeMutationCallbacks) {
  ReplayExecutionResourceBudget actionBudget;
  ASSERT_TRUE(actionBudget.reserveAction(
      ReplaySemanticActionCost{.actions = ReplayExecutionResourceBudget::kMaximumActions}));
  int actionCalls = 0;
  int inputEstimates = 0;
  int inputCalls = 0;
  const std::vector<int> actions{1};
  EXPECT_EQ(DispatchReplayFrameWithResourceBudget(
                actionBudget, actions, [](int) { return ReplaySemanticActionCost{}; },
                [&](int) { ++actionCalls; },
                [&] {
                  ++inputEstimates;
                  return ReplayInputFrameCost{};
                },
                [&] { ++inputCalls; }),
            ReplayFrameDispatchResult::ActionBudgetExceeded);
  EXPECT_THAT((std::vector<int>{actionCalls, inputEstimates, inputCalls}), ElementsAre(0, 0, 0));
  EXPECT_TRUE(actionBudget.rejected());

  ReplayExecutionResourceBudget inputBudget;
  ASSERT_TRUE(inputBudget.reserveInput(ReplayInputFrameCost{
      .inputMutations = ReplayExecutionResourceBudget::kMaximumInputMutations}));
  actionCalls = 0;
  inputCalls = 0;
  EXPECT_EQ(DispatchReplayFrameWithResourceBudget(
                inputBudget, actions, [](int) { return ReplaySemanticActionCost{}; },
                [&](int) { ++actionCalls; },
                [] { return ReplayInputFrameCost{.inputMutations = 1}; }, [&] { ++inputCalls; }),
            ReplayFrameDispatchResult::InputBudgetExceeded);
  EXPECT_THAT((std::vector<int>{actionCalls, inputCalls}), ElementsAre(1, 0));
  EXPECT_THAT(StateOf(inputBudget), Eq(BudgetState{0, 0, 1, 0, 8'192, 0, true}));
}

}  // namespace
}  // namespace donner::editor::repro
