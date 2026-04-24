// NOLINTBEGIN(llvm-include-order)
#include <cstdint>
// NOLINTEND(llvm-include-order)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "donner/editor/AddressBar.h"
#include "donner/editor/AddressBarStatus.h"
#include "imgui.h"

namespace donner::editor {

AddressBar::AddressBar() : inputBuffer_(kInputBufferSize, '\0') {}

bool AddressBar::draw() {
  bool navigated = false;

  // Ctrl/Cmd+L focuses the input.
  if (ImGui::IsKeyChordPressed(ImGuiKey_L | ImGuiMod_Shortcut)) {
    focusNextFrame_ = true;
  }

  // History dropdown.
  if (!history_.empty()) {
    ImGui::SetNextItemWidth(24.0f);
    if (ImGui::BeginCombo("##history", nullptr, ImGuiComboFlags_NoPreview)) {
      for (const auto& entry : history_) {
        if (ImGui::Selectable(entry.c_str())) {
          // Copy selected history entry into the input buffer.
          std::strncpy(inputBuffer_.data(), entry.c_str(), kInputBufferSize - 1);
          inputBuffer_[kInputBufferSize - 1] = '\0';
          pending_ = AddressBarNavigation{entry, {}};
          navigated = true;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
  }

  // Focus handling.
  if (focusNextFrame_) {
    ImGui::SetKeyboardFocusHere();
    focusNextFrame_ = false;
  }

  // URL input (full width minus space for buttons).
  ImGui::PushItemWidth(-160.0f);
  const bool submit = ImGui::InputText("##address_bar_input", inputBuffer_.data(), kInputBufferSize,
                                       ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopItemWidth();

  ImGui::SameLine();
  const bool loadClicked = ImGui::Button("Load", ImVec2(60, 0));

  // Submit on Enter or Load click. Do this BEFORE rendering the status
  // chip on its own line so a submission that transitions the chip
  // (e.g. to `kLoading`) is reflected in the same frame.
  if ((submit || loadClicked) && !navigated) {
    std::string uri(inputBuffer_.data());
    if (!uri.empty()) {
      pending_ = AddressBarNavigation{std::move(uri), {}};
      navigated = true;
    }
  }

  // Load progress bar. Rendered on its own thin strip below the input
  // row while `kLoading`. Fraction in [0,1] → fills left-to-right.
  // `nullopt` → indeterminate mode: a sliding segment animates across
  // the bar so users get "something is happening" feedback when the
  // server didn't announce a Content-Length (or on desktop where the
  // fetcher is synchronous and we never get intermediate progress).
  if (statusChip_.status == AddressBarStatus::kLoading) {
    constexpr float kBarHeight = 3.0f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImU32 bgColor = IM_COL32(40, 44, 52, 255);
    const ImU32 fgColor = IM_COL32(80, 180, 240, 255);

    drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + kBarHeight), bgColor);

    if (loadProgress_.has_value()) {
      const float frac = std::clamp(*loadProgress_, 0.0f, 1.0f);
      drawList->AddRectFilled(origin, ImVec2(origin.x + width * frac, origin.y + kBarHeight),
                              fgColor);
    } else {
      // Indeterminate: 30%-wide segment sliding across on a 1.5s cycle.
      constexpr float kCycleSeconds = 1.5f;
      constexpr float kSegmentFrac = 0.3f;
      const float t =
          std::fmod(static_cast<float>(ImGui::GetTime()), kCycleSeconds) / kCycleSeconds;
      const float segmentWidth = width * kSegmentFrac;
      // Travel from -segmentWidth to `width` so the segment fully enters
      // and fully exits, giving a cleaner "wiping across" feel than a
      // clamped slide.
      const float xStart = t * (width + segmentWidth) - segmentWidth;
      const float x0 = std::max(xStart, 0.0f);
      const float x1 = std::min(xStart + segmentWidth, width);
      if (x1 > x0) {
        drawList->AddRectFilled(ImVec2(origin.x + x0, origin.y),
                                ImVec2(origin.x + x1, origin.y + kBarHeight), fgColor);
      }
    }

    ImGui::Dummy(ImVec2(width, kBarHeight + 2.0f));
  }

  // Status chip. Rendered on its own line below the input / button row
  // with text wrapping. The previous right-of-button layout truncated
  // on narrow windows — long messages like "Got an HTML page, not an
  // SVG. Check that the URL points directly to a `.svg` file." were
  // cut off.
  if (statusChip_.status != AddressBarStatus::kEmpty) {
    ImVec4 color;
    switch (statusChip_.status) {
      case AddressBarStatus::kRendered: color = ImVec4(0.30f, 0.85f, 0.45f, 1.0f); break;
      case AddressBarStatus::kRenderedLossy: color = ImVec4(0.95f, 0.78f, 0.22f, 1.0f); break;
      case AddressBarStatus::kLoading: color = ImVec4(0.60f, 0.70f, 0.90f, 1.0f); break;
      case AddressBarStatus::kFetchError:
      case AddressBarStatus::kParseError:
      case AddressBarStatus::kPolicyDenied:
      case AddressBarStatus::kSandboxCrashed: color = ImVec4(0.92f, 0.42f, 0.38f, 1.0f); break;
      case AddressBarStatus::kEmpty: color = ImVec4(0.60f, 0.60f, 0.60f, 1.0f); break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);  // wrap at the edge of the host window
    ImGui::TextUnformatted(statusChip_.message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  }

  // If a drop payload was queued, it counts as a navigation this frame.
  if (pending_.has_value() && !navigated) {
    navigated = true;
  }

  return navigated;
}

std::optional<AddressBarNavigation> AddressBar::consumeNavigation() {
  std::optional<AddressBarNavigation> result;
  result.swap(pending_);
  return result;
}

void AddressBar::setStatus(AddressBarStatusChip chip) {
  statusChip_ = std::move(chip);
}

void AddressBar::setLoadProgress(std::optional<float> fraction) {
  loadProgress_ = fraction;
}

void AddressBar::setInitialUri(std::string_view uri) {
  const std::size_t len = std::min(uri.size(), kInputBufferSize - 1);
  std::memcpy(inputBuffer_.data(), uri.data(), len);
  inputBuffer_[len] = '\0';
}

void AddressBar::pushHistory(std::string uri) {
  // Remove existing duplicate.
  auto it = std::find(history_.begin(), history_.end(), uri);
  if (it != history_.end()) {
    history_.erase(it);
  }
  // Push to front (most-recent first).
  history_.push_front(std::move(uri));
  // Cap size.
  while (history_.size() > kHistoryCap) {
    history_.pop_back();
  }
}

void AddressBar::notifyDropPayload(std::string uri, std::vector<uint8_t> bytes) {
  // Copy into input buffer for display.
  const std::size_t len = std::min(uri.size(), kInputBufferSize - 1);
  std::memcpy(inputBuffer_.data(), uri.data(), len);
  inputBuffer_[len] = '\0';

  pending_ = AddressBarNavigation{std::move(uri), std::move(bytes)};
}

}  // namespace donner::editor
