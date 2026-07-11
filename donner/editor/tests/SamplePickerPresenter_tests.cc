#include "donner/editor/SamplePickerPresenter.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

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

}  // namespace
}  // namespace donner::editor
