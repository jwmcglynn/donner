#include "donner/editor/InspectorNumericInput.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "donner/base/StringUtils.h"
#include "donner/base/parser/LengthParser.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/ImGuiInternalIncludes.h"

namespace donner::editor {

namespace {

/// Parse a number at the start of \p text, returning it and the text that follows.
std::optional<std::pair<double, std::string_view>> ParseLeadingNumber(std::string_view text) {
  const auto result = parser::NumberParser::Parse(text);
  if (!result.hasResult() || !std::isfinite(result.result().number)) {
    return std::nullopt;
  }
  return std::make_pair(result.result().number, text.substr(result.result().consumedChars));
}

/// Text currently in the active input widget \p id, if it has one.
std::optional<std::string_view> ActiveInputText(ImGuiID id) {
  const ImGuiInputTextState* state = ImGui::GetInputTextState(id);
  if (state == nullptr) {
    return std::nullopt;
  }
  return std::string_view(state->TextA.Data, static_cast<std::size_t>(state->CurLenA));
}

/// Whether input widget \p id still has its initial text and has never been edited.
bool IsUntouchedInput(ImGuiID id, std::string_view text) {
  const ImGuiInputTextState* state = ImGui::GetInputTextState(id);
  const ImGuiContext& context = *ImGui::GetCurrentContext();
  const bool editedWhileActive = ImGui::GetActiveID() == id && context.ActiveIdHasBeenEditedBefore;
  return state != nullptr && state->InitialTextA.Data != nullptr && !editedWhileActive &&
         !ImGui::IsItemDeactivatedAfterEdit() && text == std::string_view(state->InitialTextA.Data);
}

/// Outline the last item in the destructive color and explain what it accepts.
void MarkRejectedItem(const char* hint, const EditorTheme& theme) {
  ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                      theme.destructive, theme.radiusControl, 0, 1.5f);
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetTooltip("%s", hint);
  }
}

}  // namespace

std::optional<Lengthd> ParseInspectorLength(std::string_view text, LengthUnit defaultUnit) {
  const std::string_view trimmed = StringUtils::TrimWhitespace(text);
  parser::LengthParser::Options options;
  options.unitOptional = true;
  const auto result = parser::LengthParser::Parse(trimmed, options);
  if (!result.hasResult() || result.result().consumedChars != trimmed.size() ||
      !std::isfinite(result.result().length.value)) {
    return std::nullopt;
  }
  Lengthd length = result.result().length;
  if (length.unit == LengthUnit::None) {
    length.unit = defaultUnit;
  }
  return length;
}

std::optional<double> ParseInspectorNumber(std::string_view text) {
  const auto parsed = ParseLeadingNumber(StringUtils::TrimWhitespace(text));
  if (!parsed.has_value() || !parsed->second.empty()) {
    return std::nullopt;
  }
  return parsed->first;
}

std::optional<double> ParseInspectorDegrees(std::string_view text) {
  const auto parsed = ParseLeadingNumber(StringUtils::TrimWhitespace(text));
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  const std::string_view unit = StringUtils::TrimWhitespace(parsed->second);
  if (!unit.empty() && !StringUtils::EqualsLowercase(unit, std::string_view("deg"))) {
    return std::nullopt;
  }
  return parsed->first;
}

InspectorNumericEdit DragInspectorNumber(const char* label, double* value, float speed,
                                         const char* format, const InspectorTextParser& parse,
                                         const char* hint, const EditorTheme& theme) {
  const ImGuiID id = ImGui::GetID(label);
  // ImGui keeps typed text in its own state; only frames that began in text entry carry it.
  const bool typing = ImGui::TempInputIsActive(id);
  const double before = *value;
  InspectorNumericEdit edit;
  edit.changed =
      ImGui::DragScalar(label, ImGuiDataType_Double, value, speed, nullptr, nullptr, format);
  const std::optional<std::string_view> text = typing ? ActiveInputText(id) : std::nullopt;
  edit.typing = text.has_value();
  if (edit.typing && !IsUntouchedInput(id, *text)) {
    const std::optional<double> parsed = parse.parse(parse.context, *text);
    edit.rejected = !parsed.has_value();
    *value = parsed.value_or(before);
    edit.changed = *value != before;
  }
  if (edit.rejected) {
    MarkRejectedItem(hint, theme);
  }
  return edit;
}

InspectorLengthEdit InputInspectorLength(const char* label, const Lengthd& current,
                                         LengthUnit bareNumberUnit, bool allowNegative,
                                         const EditorTheme& theme) {
  // A zero length serializes as "0" followed by exactly its unit suffix.
  const RcString zeroWithUnit = Lengthd(0.0, current.unit).toRcString();
  const std::string_view unit = std::string_view(zeroWithUnit).substr(1);
  std::array<char, 64> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.1f%.*s", current.value,
                static_cast<int>(unit.size()), unit.data());

  InspectorLengthEdit edit;
  (void)ImGui::InputText(label, buffer.data(), buffer.size(), ImGuiInputTextFlags_AutoSelectAll);
  const ImGuiID id = ImGui::GetItemID();
  const bool editing = ImGui::IsItemActive() || ImGui::IsItemDeactivated();
  const std::optional<std::string_view> text = editing ? ActiveInputText(id) : std::nullopt;
  edit.typing = text.has_value();
  if (edit.typing && !IsUntouchedInput(id, *text)) {
    std::optional<Lengthd> parsed = ParseInspectorLength(*text, bareNumberUnit);
    if (parsed.has_value() && !allowNegative && parsed->value < 0.0) {
      parsed.reset();
    }
    edit.rejected = !parsed.has_value();
    if (parsed.has_value() && *parsed != current) {
      edit.typed = parsed;
    }
  }
  if (edit.rejected) {
    MarkRejectedItem(kInspectorLengthHint, theme);
  }
  return edit;
}

}  // namespace donner::editor
