#include "donner/editor/SamplePickerPresenter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/editor/EditorSampleCatalog.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {
namespace {

TEST(SamplePickerPresenter, NarrowLayoutUsesOneTouchSizedColumn) {
  const SamplePickerLayout layout = ComputeSamplePickerLayout(480.0f, 4u);

  EXPECT_EQ(layout.mode, SamplePickerLayoutMode::Narrow);
  EXPECT_EQ(layout.columns, 1u);
  EXPECT_EQ(layout.rows, 4u);
  EXPECT_GE(layout.cardWidth, 0.0f);
  EXPECT_GE(layout.cardHeight, kSamplePickerMinTouchTarget);
  EXPECT_FLOAT_EQ(layout.cardHeight, 96.0f);
}

TEST(SamplePickerPresenter, WideLayoutUsesCompactBoundedColumns) {
  const SamplePickerLayout layout = ComputeSamplePickerLayout(1024.0f, 4u);

  EXPECT_EQ(layout.mode, SamplePickerLayoutMode::Wide);
  EXPECT_EQ(layout.columns, 3u);
  EXPECT_EQ(layout.rows, 2u);
  EXPECT_GT(layout.cardWidth, 0.0f);
  EXPECT_GE(layout.cardHeight, kSamplePickerMinTouchTarget);
  EXPECT_FLOAT_EQ(layout.cardHeight, 96.0f);
  EXPECT_FLOAT_EQ(layout.cardWidth * 3.0f + 12.0f * 2.0f, 1024.0f);
}

TEST(SamplePickerPresenter, LayoutClampsInvalidWidthAndVisibleCount) {
  const SamplePickerLayout layout =
      ComputeSamplePickerLayout(-20.0f, kSamplePickerMaxVisibleSamples + 4u);

  EXPECT_EQ(layout.mode, SamplePickerLayoutMode::Narrow);
  EXPECT_EQ(layout.columns, 1u);
  EXPECT_EQ(layout.rows, kSamplePickerMaxVisibleSamples);
  EXPECT_FLOAT_EQ(layout.cardWidth, 0.0f);

  // Non-finite widths clamp to zero as well.
  const SamplePickerLayout infiniteLayout =
      ComputeSamplePickerLayout(std::numeric_limits<float>::infinity(), 4u);
  EXPECT_EQ(infiniteLayout.mode, SamplePickerLayoutMode::Narrow);
  EXPECT_EQ(infiniteLayout.columns, 1u);
  EXPECT_FLOAT_EQ(infiniteLayout.cardWidth, 0.0f);
}

TEST(SamplePickerPresenter, EmptyCatalogLayoutHasNoRowsOrColumns) {
  const SamplePickerLayout layout = ComputeSamplePickerLayout(800.0f, 0u);

  EXPECT_EQ(layout.columns, 0u);
  EXPECT_EQ(layout.rows, 0u);
  EXPECT_FLOAT_EQ(layout.cardWidth, 0.0f);
}

TEST(SamplePickerPresenter, DescriptionsCoverCatalogAndFallback) {
  constexpr std::array<std::string_view, 4> kSampleIds = {"donner-splash", "basic-shapes",
                                                          "text-style", "gradients-clip"};
  for (const std::string_view id : kSampleIds) {
    EXPECT_FALSE(SamplePickerDescription(id).empty());
  }
  EXPECT_EQ(SamplePickerDescription("unknown"), "Built-in SVG sample");
}

TEST(SamplePickerPresenter, CommandsProduceSemanticActions) {
  SamplePickerActions actions;

  ApplySamplePickerCommand(true, SamplePickerCommand::Dismiss, {}, &actions);
  EXPECT_TRUE(actions.dismiss);

  actions = SamplePickerActions{};
  ApplySamplePickerCommand(true, SamplePickerCommand::OpenFile, {}, &actions);
  EXPECT_TRUE(actions.openFile);

  actions = SamplePickerActions{};

  ApplySamplePickerCommand(false, SamplePickerCommand::LoadSample, "basic-shapes", &actions);
  EXPECT_FALSE(actions.loadSample);
  EXPECT_TRUE(actions.sampleId.empty());

  ApplySamplePickerCommand(true, SamplePickerCommand::LoadSample, "basic-shapes", &actions);
  EXPECT_TRUE(actions.loadSample);
  EXPECT_EQ(actions.sampleId, "basic-shapes");
  EXPECT_FALSE(actions.openGitHub);

  actions = SamplePickerActions{};
  ApplySamplePickerCommand(true, SamplePickerCommand::OpenGitHub, {}, &actions);
  EXPECT_FALSE(actions.loadSample);
  EXPECT_TRUE(actions.openGitHub);
}

TEST(SamplePickerPresenter, CommandsIgnoreEmptySampleAndNullAccumulator) {
  SamplePickerActions actions;
  ApplySamplePickerCommand(true, SamplePickerCommand::LoadSample, {}, &actions);
  EXPECT_FALSE(actions.loadSample);
  EXPECT_TRUE(actions.sampleId.empty());

  ApplySamplePickerCommand(true, SamplePickerCommand::LoadSample, "unknown", &actions);
  EXPECT_FALSE(actions.loadSample);
  EXPECT_TRUE(actions.sampleId.empty());

  ApplySamplePickerCommand(true, SamplePickerCommand::OpenGitHub, {}, nullptr);
  EXPECT_FALSE(actions.openGitHub);
}

// ---------------------------------------------------------------------------
// Rendered picker behavior (headless ImGui)
// ---------------------------------------------------------------------------

/// True when no action flag is set on @p actions.
bool NoActions(const SamplePickerActions& actions) {
  return !actions.dismiss && !actions.openFile && !actions.loadSample && !actions.openGitHub &&
         actions.sampleId.empty();
}

/// Accumulate every edge-triggered flag from @p frame into @p merged.
void MergeActions(SamplePickerActions& merged, const SamplePickerActions& frame) {
  merged.dismiss = merged.dismiss || frame.dismiss;
  merged.openFile = merged.openFile || frame.openFile;
  merged.loadSample = merged.loadSample || frame.loadSample;
  merged.openGitHub = merged.openGitHub || frame.openGitHub;
  if (!frame.sampleId.empty()) {
    merged.sampleId = frame.sampleId;
  }
}

class SamplePickerPresenterImGuiTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1200.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigMacOSXBehaviors = false;
    io.Fonts->Build();
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    context_ = nullptr;
  }

  /// Render one picker frame in a host window of the given width.
  SamplePickerActions Frame(const SamplePickerState& state,
                            const SamplePickerThumbnailProvider& provider, float hostWidth,
                            const ImVec2& mouse = ImVec2(-100.0f, -100.0f),
                            bool mouseDown = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1200.0f, 800.0f);
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, mouseDown);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(hostWidth, 760.0f), ImGuiCond_Always);
    ImGui::Begin("##sample_picker_host", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    const SamplePickerActions actions = presenter_.render(state, provider);
    ImGui::End();
    ImGui::Render();
    const ImDrawData* drawData = ImGui::GetDrawData();
    lastVertexCount_ = drawData != nullptr ? drawData->TotalVtxCount : 0;
    return actions;
  }

  /// Simulate a full left-button click (press frame + release frame) at
  /// @p mouse and return the merged actions of both frames.
  SamplePickerActions Click(const SamplePickerState& state,
                            const SamplePickerThumbnailProvider& provider, float hostWidth,
                            const ImVec2& mouse) {
    SamplePickerActions merged = Frame(state, provider, hostWidth, mouse, /*mouseDown=*/true);
    MergeActions(merged, Frame(state, provider, hostWidth, mouse, /*mouseDown=*/false));
    return merged;
  }

  SamplePickerPresenter presenter_;
  int lastVertexCount_ = 0;

private:
  ImGuiContext* context_ = nullptr;
};

TEST_F(SamplePickerPresenterImGuiTest, HiddenPickerEmitsNothingAndSkipsDrawing) {
  SamplePickerState hidden;
  hidden.visible = false;

  EXPECT_TRUE(NoActions(Frame(hidden, {}, 1024.0f)));
  const int hiddenVertexCount = lastVertexCount_;

  EXPECT_TRUE(NoActions(Frame(SamplePickerState{}, {}, 1024.0f)));
  EXPECT_GT(lastVertexCount_, hiddenVertexCount);
}

TEST_F(SamplePickerPresenterImGuiTest, RenderRequestsOneThumbnailPerVisibleCatalogSample) {
  const std::span<const EditorSample> samples = GetEditorSampleCatalog();
  ASSERT_FALSE(samples.empty());

  SamplePickerState state;
  state.selectedSampleId = samples.front().id;  // Exercises the selected-card styling.

  std::vector<std::pair<std::string, std::size_t>> requests;
  const SamplePickerThumbnailProvider provider =
      [&requests](const EditorSample& sample, std::size_t index) {
        requests.emplace_back(std::string(sample.id), index);
        SamplePickerThumbnail thumbnail;
        // Alternate landscape/portrait art so both letterbox arms are drawn.
        thumbnail.texture = static_cast<ImTextureID>(0x1234);
        thumbnail.aspectRatio = index % 2 == 0 ? 1.8f : 0.5f;
        return thumbnail;
      };

  // A wide pane lays the catalog out in a multi-column grid; rendering asks
  // the provider for exactly one thumbnail per visible sample, in order.
  EXPECT_TRUE(NoActions(Frame(state, provider, 1024.0f)));
  const std::size_t visibleCount = std::min(samples.size(), kSamplePickerMaxVisibleSamples);
  ASSERT_EQ(requests.size(), visibleCount);
  for (std::size_t i = 0; i < visibleCount; ++i) {
    EXPECT_EQ(requests[i].first, std::string_view(samples[i].id));
    EXPECT_EQ(requests[i].second, i);
  }

  // Without a provider the cards render placeholder art and still emit no
  // actions; the frame draws fewer vertices than the thumbnail-backed frame.
  const int thumbnailVertexCount = lastVertexCount_;
  EXPECT_TRUE(NoActions(Frame(state, {}, 1024.0f)));
  EXPECT_LT(lastVertexCount_, thumbnailVertexCount);
}

TEST_F(SamplePickerPresenterImGuiTest, ClickingDismissButtonEmitsDismiss) {
  const SamplePickerState state;
  constexpr float kHostWidth = 1024.0f;

  Frame(state, {}, kHostWidth);  // Warm-up frame for hover state.

  // The dismiss button is a 44px square hugging the top-right content edge;
  // probe leftward along its row.
  SamplePickerActions dismissed;
  for (float x = 1014.0f; x >= 900.0f; x -= 6.0f) {
    const SamplePickerActions actions = Click(state, {}, kHostWidth, ImVec2(x, 30.0f));
    EXPECT_FALSE(actions.openFile);
    EXPECT_FALSE(actions.loadSample);
    EXPECT_FALSE(actions.openGitHub);
    if (actions.dismiss) {
      dismissed = actions;
      break;
    }
  }

  EXPECT_TRUE(dismissed.dismiss) << "Dismiss button not found along the top-right edge";
}

TEST_F(SamplePickerPresenterImGuiTest, ClickingActionButtonsEmitsOpenFileAndOpenGitHub) {
  const SamplePickerState state;
  constexpr float kHostWidth = 1024.0f;

  Frame(state, {}, kHostWidth);

  // The "Open SVG" button hugs the left content edge below the heading text;
  // its vertical position depends on font metrics, so probe downward.
  float openRowY = -1.0f;
  for (float y = 56.0f; y <= 400.0f; y += 6.0f) {
    const SamplePickerActions actions = Click(state, {}, kHostWidth, ImVec2(60.0f, y));
    EXPECT_FALSE(actions.dismiss);
    EXPECT_FALSE(actions.loadSample);
    if (actions.openFile) {
      openRowY = y;
      break;
    }
  }
  ASSERT_GT(openRowY, 0.0f) << "Open SVG button not found along the left edge";

  // The GitHub action sits on the same row, right of the 112px-wide Open SVG
  // button (8px gap).
  bool openedGitHub = false;
  for (float x = 132.0f; x <= 300.0f; x += 12.0f) {
    const SamplePickerActions actions = Click(state, {}, kHostWidth, ImVec2(x, openRowY));
    EXPECT_FALSE(actions.dismiss);
    EXPECT_FALSE(actions.loadSample);
    if (actions.openGitHub) {
      openedGitHub = true;
      break;
    }
  }
  EXPECT_TRUE(openedGitHub) << "GitHub action button not found on the Open SVG row";
}

TEST_F(SamplePickerPresenterImGuiTest, ClickingFirstSampleCardLoadsThatSample) {
  const std::span<const EditorSample> samples = GetEditorSampleCatalog();
  ASSERT_FALSE(samples.empty());

  SamplePickerState state;
  if (samples.size() > 1u) {
    state.selectedSampleId = samples[1].id;  // Selected styling on an unclicked card.
  }

  // A narrow pane stacks full-width cards in one column, so the first card
  // encountered scanning down the pane center is the first catalog entry.
  constexpr float kHostWidth = 500.0f;
  Frame(state, {}, kHostWidth);

  SamplePickerActions loaded;
  for (float y = 56.0f; y <= 740.0f; y += 8.0f) {
    const SamplePickerActions actions = Click(state, {}, kHostWidth, ImVec2(250.0f, y));
    EXPECT_FALSE(actions.dismiss);
    if (actions.loadSample) {
      loaded = actions;
      break;
    }
  }

  ASSERT_TRUE(loaded.loadSample) << "No sample card found scanning down the narrow pane";
  EXPECT_EQ(loaded.sampleId, std::string_view(samples.front().id));
}

}  // namespace
}  // namespace donner::editor
