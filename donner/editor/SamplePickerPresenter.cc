#include "donner/editor/SamplePickerPresenter.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "donner/editor/EditorTheme.h"
#include "donner/editor/ExternalUrlLauncher.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {
namespace {

constexpr float kGridGap = 12.0f;
constexpr float kMinimumCardWidth = 220.0f;
constexpr float kCardHeight = 96.0f;
constexpr float kThumbnailWidth = 104.0f;
constexpr float kThumbnailHeight = 64.0f;
constexpr float kHeadingSpacing = 8.0f;

std::size_t BoundedSampleCount(std::size_t sampleCount) noexcept {
  return std::min(sampleCount, kSamplePickerMaxVisibleSamples);
}

float SampleTextWidth(float cardWidth) {
  const EditorTheme& theme = EditorTheme::Active();
  return std::max(1.0f, cardWidth - kThumbnailWidth - 2.0f * theme.space2 - theme.space3);
}

ImVec2 WrappedTextSize(std::string_view text, float width) {
  return ImGui::CalcTextSize(text.data(), text.data() + text.size(), false, width);
}

float SampleCardHeight(std::span<const EditorSample> samples, float cardWidth) {
  const EditorTheme& theme = EditorTheme::Active();
  const float textWidth = SampleTextWidth(cardWidth);
  float height = kCardHeight;
  for (const EditorSample& sample : samples.first(BoundedSampleCount(samples.size()))) {
    const float textHeight = WrappedTextSize(sample.title, textWidth).y + 2.0f +
                             WrappedTextSize(SamplePickerDescription(sample.id), textWidth).y;
    height = std::max(height, textHeight + 2.0f * theme.space2);
  }
  return height;
}

void DrawSampleButton(const EditorSample& sample, std::string_view description,
                      const SamplePickerState& state, float width, float height,
                      const SamplePickerThumbnail& thumbnail, SamplePickerActions* actions,
                      std::size_t index) {
  const EditorTheme& theme = EditorTheme::Active();
  ImGui::PushID(static_cast<int>(index));
  const bool clicked = ImGui::InvisibleButton("##sample", ImVec2(width, height));
  if (ImGui::IsItemVisible()) {
    actions->visibleSampleIndices.push_back(index);
  }
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
  const float textWidth = SampleTextWidth(width);
  const float titleHeight = WrappedTextSize(sample.title, textWidth).y;
  const float textHeight = titleHeight + 2.0f + WrappedTextSize(description, textWidth).y;
  const float titleY = min.y + (height - textHeight) * 0.5f;
  drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, titleY),
                    theme.textPrimary, sample.title.data(),
                    sample.title.data() + sample.title.size(), textWidth);
  drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(textX, titleY + titleHeight + 2.0f), theme.textMuted, description.data(),
                    description.data() + description.size(), textWidth);
  drawList->PopClipRect();

  if (clicked) {
    ApplySamplePickerCommand(true, SamplePickerCommand::LoadSample, sample.id, actions);
  }
  ImGui::PopID();
}

void DrawSamplePickerHeader(const SamplePickerState& state, const EditorTheme& theme,
                            float availableWidth, SamplePickerActions* actions) {
  const float dismissSize = kSamplePickerMinTouchTarget;
  ImGui::SetCursorPosX(std::max(0.0f, availableWidth - dismissSize));
  if (ImGui::Button("X##dismiss_samples", ImVec2(dismissSize, dismissSize))) {
    ApplySamplePickerCommand(true, SamplePickerCommand::Dismiss, {}, actions);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Close samples");
  }
  ImGui::TextUnformatted("Donner");
  ImGui::Spacing();
  ImGui::TextWrapped("Edit SVG graphics and source together.");
  if (state.loading) {
    ImGui::TextUnformatted("Loading sample...");
  }
  if (!state.error.empty()) {
    ImGui::TextWrapped("%.*s", static_cast<int>(state.error.size()), state.error.data());
  }
  ImGui::Dummy(ImVec2(0.0f, kHeadingSpacing));

  const float newWidth = std::max(96.0f, ImGui::CalcTextSize("New SVG").x + 2.0f * theme.space4);
  if (ImGui::Button("New SVG", ImVec2(newWidth, kSamplePickerMinTouchTarget))) {
    ApplySamplePickerCommand(true, SamplePickerCommand::NewDocument, {}, actions);
  }
  ImGui::SameLine(0.0f, theme.space2);
  const float openWidth = std::max(112.0f, ImGui::CalcTextSize("Open SVG").x + 2.0f * theme.space4);
  if (ImGui::Button("Open SVG", ImVec2(openWidth, kSamplePickerMinTouchTarget))) {
    ApplySamplePickerCommand(true, SamplePickerCommand::OpenFile, {}, actions);
  }
  ImGui::SameLine(0.0f, theme.space2);
  // The GitHub action reads as a hyperlink (underlined, link-colored, hand
  // cursor) and reports the same edge-triggered action the old button did.
  const float linkY =
      ImGui::GetCursorPosY() + (kSamplePickerMinTouchTarget - ImGui::GetTextLineHeight()) * 0.5f;
  ImGui::SetCursorPosY(linkY);
  if (ImGui::TextLink(ExternalUrlValue(ExternalUrlTarget::DonnerRepository).data())) {
    ApplySamplePickerCommand(true, SamplePickerCommand::OpenGitHub, {}, actions);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open on GitHub");
  }
  ImGui::Dummy(ImVec2(0.0f, theme.space3));
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
  if (sampleId == "animation") {
    return "Hover, press, and click to animate";
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
    case SamplePickerCommand::NewDocument: actions->newDocument = true; return;
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
  SamplePickerLayout layout = ComputeSamplePickerLayout(availableWidth, samples.size());
  layout.cardHeight = SampleCardHeight(samples, layout.cardWidth);

  DrawSamplePickerHeader(state, theme, availableWidth, &actions);

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
