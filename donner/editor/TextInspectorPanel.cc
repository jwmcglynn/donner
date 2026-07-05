#include "donner/editor/TextInspectorPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGTextElement.h"

namespace donner::editor {

namespace {

/// Copies `value` into a fixed-size, null-terminated buffer.
template <std::size_t N>
void AssignBuffer(std::array<char, N>& buffer, std::string_view value) {
  const std::size_t count = std::min(value.size(), N - 1u);
  std::memcpy(buffer.data(), value.data(), count);
  buffer[count] = '\0';
}

/// Formats an RGBA float color (each component 0..1) as `#rrggbb` (opaque) or
/// `#rrggbbaa` (translucent).
std::string FormatColor(const std::array<float, 4>& color) {
  auto toByte = [](float channel) {
    const float clamped = std::clamp(channel, 0.0f, 1.0f);
    return static_cast<int>(std::lround(clamped * 255.0f));
  };
  char out[10];
  if (toByte(color[3]) >= 255) {
    std::snprintf(out, sizeof(out), "#%02x%02x%02x", toByte(color[0]), toByte(color[1]),
                  toByte(color[2]));
  } else {
    std::snprintf(out, sizeof(out), "#%02x%02x%02x%02x", toByte(color[0]), toByte(color[1]),
                  toByte(color[2]), toByte(color[3]));
  }
  return std::string(out);
}

/// Returns the lone selected `<text>` element, or `std::nullopt`.
std::optional<svg::SVGElement> SingleSelectedText(EditorApp& app) {
  const std::vector<svg::SVGElement>& selection = app.selectedElements();
  if (selection.size() != 1u) {
    return std::nullopt;
  }
  if (selection.front().type() != svg::ElementType::Text) {
    return std::nullopt;
  }
  return selection.front();
}

}  // namespace

void TextInspectorPanel::syncBuffersFromSelection(const svg::SVGElement& text) {
  // §concurrent-dom: the live editor keeps the document in ThreadingMode::ConcurrentDom, so reading
  // textContent()/attributes (raw ECS access) from this UI-thread render path needs a scoped read
  // access - without it `SVGTextContentElement::textContent()` trips the access assert and aborts.
  text.withReadAccess([&](svg::DocumentReadAccess&, EntityHandle) {
    if (text.isa<svg::SVGTextElement>()) {
      AssignBuffer(contentBuffer_,
                   std::string_view(text.cast<svg::SVGTextElement>().textContent()));
    } else {
      AssignBuffer(contentBuffer_, "");
    }

    if (auto family = text.getAttribute(xml::XMLQualifiedNameRef("font-family"))) {
      AssignBuffer(fontFamilyBuffer_, std::string_view(*family));
    } else {
      AssignBuffer(fontFamilyBuffer_, "");
    }

    if (auto size = text.getAttribute(xml::XMLQualifiedNameRef("font-size"))) {
      AssignBuffer(fontSizeBuffer_, std::string_view(*size));
    } else {
      AssignBuffer(fontSizeBuffer_, "");
    }

    if (auto strokeWidth = text.getAttribute(xml::XMLQualifiedNameRef("stroke-width"))) {
      AssignBuffer(strokeWidthBuffer_, std::string_view(*strokeWidth));
    } else {
      AssignBuffer(strokeWidthBuffer_, "");
    }
  });

  contentDirty_ = false;
}

bool TextInspectorPanel::commitPendingContent(EditorApp& liveApp) {
  if (!contentDirty_ || !trackedElement_.has_value()) {
    return false;
  }
  liveApp.applyMutation(
      EditorCommand::SetTextContentCommand(*trackedElement_, std::string(contentBuffer_.data())));
  contentDirty_ = false;
  return true;
}

bool TextInspectorPanel::render(EditorApp* liveApp, double nowSeconds) {
  if (liveApp == nullptr) {
    return false;
  }

  std::optional<svg::SVGElement> text = SingleSelectedText(*liveApp);
  if (!text.has_value()) {
    // Commit any pending edit before the panel goes away so no keystrokes are
    // lost when the selection changes.
    bool queued = false;
    if (trackedElement_.has_value()) {
      queued = commitPendingContent(*liveApp);
    }
    trackedElement_.reset();
    return queued;
  }

  bool queuedMutation = false;

  if (!trackedElement_.has_value() || *trackedElement_ != *text) {
    // Commit edits to the previously-tracked element before switching.
    if (trackedElement_.has_value()) {
      queuedMutation = commitPendingContent(*liveApp) || queuedMutation;
    }
    trackedElement_ = text;
    syncBuffersFromSelection(*text);
  }

  ImGui::SeparatorText("Text");

  // Content: debounced into one undo entry on focus loss or idle.
  if (ImGui::InputText("Content", contentBuffer_.data(), contentBuffer_.size())) {
    contentDirty_ = true;
    contentLastEditSeconds_ = nowSeconds;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    queuedMutation = commitPendingContent(*liveApp) || queuedMutation;
  } else if (contentDirty_ && (nowSeconds - contentLastEditSeconds_) >= kContentCommitIdleSeconds) {
    queuedMutation = commitPendingContent(*liveApp) || queuedMutation;
  }

  // Font family.
  ImGui::InputText("Font family", fontFamilyBuffer_.data(), fontFamilyBuffer_.size());
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    queuedMutation =
        liveApp->setAttributeOnSelection("font-family", fontFamilyBuffer_.data()) || queuedMutation;
  }

  // Font size.
  ImGui::InputText("Font size", fontSizeBuffer_.data(), fontSizeBuffer_.size(),
                   ImGuiInputTextFlags_CharsDecimal);
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    queuedMutation =
        liveApp->setAttributeOnSelection("font-size", fontSizeBuffer_.data()) || queuedMutation;
  }

  // Fill color picker.
  if (ImGui::ColorEdit4("Fill", fillColor_.data())) {
    queuedMutation =
        liveApp->setAttributeOnSelection("fill", FormatColor(fillColor_)) || queuedMutation;
  }

  // Stroke color + width.
  if (ImGui::ColorEdit4("Stroke", strokeColor_.data())) {
    queuedMutation =
        liveApp->setAttributeOnSelection("stroke", FormatColor(strokeColor_)) || queuedMutation;
  }
  ImGui::InputText("Stroke width", strokeWidthBuffer_.data(), strokeWidthBuffer_.size(),
                   ImGuiInputTextFlags_CharsDecimal);
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    queuedMutation = liveApp->setAttributeOnSelection("stroke-width", strokeWidthBuffer_.data()) ||
                     queuedMutation;
  }

  return queuedMutation;
}

}  // namespace donner::editor
