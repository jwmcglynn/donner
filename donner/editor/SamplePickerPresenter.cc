#include "donner/editor/SamplePickerPresenter.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {
namespace {

constexpr float kGridGap = 12.0f;
constexpr float kMinimumCardWidth = 220.0f;
constexpr float kCardHeight = 96.0f;
constexpr float kThumbnailWidth = 104.0f;
constexpr float kThumbnailHeight = 64.0f;
constexpr float kHeadingSpacing = 8.0f;
#ifdef __EMSCRIPTEN__
constexpr std::string_view kGitHubActionLabel = "View on GitHub";
#else
constexpr std::string_view kGitHubActionLabel = "Copy GitHub Link";
#endif

std::size_t BoundedSampleCount(std::size_t sampleCount) noexcept {
  return std::min(sampleCount, kSamplePickerMaxVisibleSamples);
}

void DrawSampleButton(const EditorSample& sample, std::string_view description,
                      const SamplePickerState& state, float width, float height,
                      const SamplePickerThumbnail& thumbnail, SamplePickerActions* actions,
                      std::size_t index) {
  const EditorTheme& theme = EditorTheme::Active();
  ImGui::PushID(static_cast<int>(index));
  const bool clicked = ImGui::InvisibleButton("##sample", ImVec2(width, height));
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  const bool selected = state.selectedSampleId == sample.id;

  ImU32 fill = theme.surfaceRaised;
  if (active || selected) {
    fill = theme.surfaceActive;
  } else if (hovered) {
    fill = theme.surfaceHover;
  }
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(min, max, fill, theme.radiusControl);
  drawList->AddRect(min, max, selected ? theme.accentDefault : theme.borderSubtle,
                    theme.radiusControl, 0, selected ? 2.0f : 1.0f);

  drawList->PushClipRect(min, max, true);
  const ImVec2 thumbnailSlotMin(min.x + theme.space2,
                                min.y + std::max(0.0f, (height - kThumbnailHeight) * 0.5f));
  const ImVec2 thumbnailSlotMax(thumbnailSlotMin.x + kThumbnailWidth,
                                thumbnailSlotMin.y + kThumbnailHeight);
  drawList->AddRectFilled(thumbnailSlotMin, thumbnailSlotMax, theme.surfaceCanvas,
                          theme.radiusControl);
  if (thumbnail.texture != 0) {
    const float aspectRatio = std::max(0.01f, thumbnail.aspectRatio);
    const float previewWidth = std::min(kThumbnailWidth, kThumbnailHeight * aspectRatio);
    const float previewHeight = std::min(kThumbnailHeight, previewWidth / aspectRatio);
    const ImVec2 thumbnailMin(thumbnailSlotMin.x + (kThumbnailWidth - previewWidth) * 0.5f,
                              thumbnailSlotMin.y + (kThumbnailHeight - previewHeight) * 0.5f);
    const ImVec2 thumbnailMax(thumbnailMin.x + previewWidth, thumbnailMin.y + previewHeight);
    drawList->AddImage(thumbnail.texture, thumbnailMin, thumbnailMax, ImVec2(0.0f, 0.0f),
                       ImVec2(static_cast<float>(thumbnail.uvBottomRight.x),
                              static_cast<float>(thumbnail.uvBottomRight.y)));
  }
  drawList->AddRect(thumbnailSlotMin, thumbnailSlotMax, theme.borderSubtle, theme.radiusControl);
  const float textX = thumbnailSlotMax.x + theme.space3;
  const float titleY = min.y + (height - ImGui::GetTextLineHeight() * 2.0f - 2.0f) * 0.5f;
  drawList->AddText(ImVec2(textX, titleY), theme.textPrimary, sample.title.data(),
                    sample.title.data() + sample.title.size());
  drawList->AddText(ImVec2(textX, titleY + ImGui::GetTextLineHeight() + 2.0f), theme.textMuted,
                    description.data(), description.data() + description.size());
  drawList->PopClipRect();

  if (clicked) {
    ApplySamplePickerCommand(true, SamplePickerCommand::LoadSample, sample.id, actions);
  }
  ImGui::PopID();
}

}  // namespace

SamplePickerLayout ComputeSamplePickerLayout(float availableWidth,
                                             std::size_t sampleCount) noexcept {
  SamplePickerLayout layout;
  const std::size_t boundedCount = BoundedSampleCount(sampleCount);
  const float width = std::isfinite(availableWidth) ? std::max(0.0f, availableWidth) : 0.0f;
  if (boundedCount == 0u) {
    layout.columns = 0u;
    layout.rows = 0u;
    return layout;
  }

  if (width < kSamplePickerNarrowBreakpoint) {
    layout.mode = SamplePickerLayoutMode::Narrow;
    layout.columns = 1u;
  } else {
    layout.mode = SamplePickerLayoutMode::Wide;
    const auto columnEstimate = static_cast<std::size_t>(
        std::max(1.0f, std::floor((width + kGridGap) / (kMinimumCardWidth + kGridGap))));
    layout.columns = std::clamp(columnEstimate, std::size_t{2}, kSamplePickerMaxColumns);
    layout.columns = std::min(layout.columns, boundedCount);
  }

  layout.rows = (boundedCount + layout.columns - 1u) / layout.columns;
  layout.cardWidth = std::max(0.0f, (width - kGridGap * static_cast<float>(layout.columns - 1u)) /
                                        static_cast<float>(layout.columns));
  layout.cardHeight = kCardHeight;
  return layout;
}

std::string_view SamplePickerDescription(std::string_view sampleId) noexcept {
  if (sampleId == "donner-splash") {
    return "A quick look at Donner";
  }
  if (sampleId == "basic-shapes") {
    return "Shapes, fills, and strokes";
  }
  if (sampleId == "text-style") {
    return "Text and inherited styles";
  }
  if (sampleId == "gradients-clip") {
    return "Gradients and clipping";
  }
  return "Built-in SVG sample";
}

void ApplySamplePickerCommand(bool activated, SamplePickerCommand command,
                              std::string_view sampleId, SamplePickerActions* actions) {
  if (!activated || actions == nullptr) {
    return;
  }

  switch (command) {
    case SamplePickerCommand::Dismiss: actions->dismiss = true; return;
    case SamplePickerCommand::OpenFile: actions->openFile = true; return;
    case SamplePickerCommand::LoadSample:
      if (FindEditorSample(sampleId) != nullptr) {
        actions->loadSample = true;
        actions->sampleId.assign(sampleId.data(), sampleId.size());
      }
      return;
    case SamplePickerCommand::OpenGitHub: actions->openGitHub = true; return;
  }
}

SamplePickerActions SamplePickerPresenter::render(
    const SamplePickerState& state, const SamplePickerThumbnailProvider& thumbnailProvider) const {
  SamplePickerActions actions;
  if (!state.visible) {
    return actions;
  }

  const EditorTheme& theme = EditorTheme::Active();
  const float availableWidth = ImGui::GetContentRegionAvail().x;
  const std::span<const EditorSample> samples = GetEditorSampleCatalog();
  const SamplePickerLayout layout = ComputeSamplePickerLayout(availableWidth, samples.size());

  const float dismissSize = kSamplePickerMinTouchTarget;
  ImGui::SetCursorPosX(std::max(0.0f, availableWidth - dismissSize));
  if (ImGui::Button("X##dismiss_samples", ImVec2(dismissSize, dismissSize))) {
    ApplySamplePickerCommand(true, SamplePickerCommand::Dismiss, {}, &actions);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Close samples");
  }
  ImGui::TextUnformatted("Donner");
  ImGui::Spacing();
  ImGui::TextWrapped("Edit SVG graphics and source together.");
  ImGui::Dummy(ImVec2(0.0f, kHeadingSpacing));

  const float openWidth = std::max(112.0f, ImGui::CalcTextSize("Open SVG").x + 2.0f * theme.space4);
  if (ImGui::Button("Open SVG", ImVec2(openWidth, kSamplePickerMinTouchTarget))) {
    ApplySamplePickerCommand(true, SamplePickerCommand::OpenFile, {}, &actions);
  }
  ImGui::SameLine(0.0f, theme.space2);
  const float githubWidth =
      std::max(128.0f, ImGui::CalcTextSize(kGitHubActionLabel.data()).x + 2.0f * theme.space4);
  if (ImGui::Button(kGitHubActionLabel.data(), ImVec2(githubWidth, kSamplePickerMinTouchTarget))) {
    ApplySamplePickerCommand(true, SamplePickerCommand::OpenGitHub, {}, &actions);
  }
  ImGui::Dummy(ImVec2(0.0f, theme.space3));

  for (std::size_t index = 0; index < layout.rows * layout.columns; ++index) {
    if (index >= samples.size() || index >= kSamplePickerMaxVisibleSamples) {
      break;
    }
    if (index > 0u && index % layout.columns != 0u) {
      ImGui::SameLine(0.0f, kGridGap);
    }
    const EditorSample& sample = samples[index];
    const SamplePickerThumbnail thumbnail =
        thumbnailProvider ? thumbnailProvider(sample, index) : SamplePickerThumbnail{};
    DrawSampleButton(sample, SamplePickerDescription(sample.id), state, layout.cardWidth,
                     layout.cardHeight, thumbnail, &actions, index);
  }

  return actions;
}

}  // namespace donner::editor
