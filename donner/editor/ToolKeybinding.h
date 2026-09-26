#pragma once
/// @file
///
/// Canonical editor tool identity plus its industry-standard single-key
/// shortcut and display label. Kept as a tiny pure header (no ImGui, no
/// `EditorShell`) so the toolbar tooltip text and the keyboard shortcut handler
/// share one source of truth and the mapping can be unit-tested without a live
/// ImGui/GL context.

#include <array>
#include <cstdint>
#include <string_view>

namespace donner::editor {

/// The editor's interactive tools. Mirrors `EditorShell::ActiveTool` one-to-one;
/// kept as a standalone enum so the keybinding mapping is testable in isolation.
enum class ToolId : std::uint8_t {
  Select,      ///< Selection / Arrow tool.
  Pen,         ///< Pen (path) tool.
  Text,        ///< Type / Text tool.
  Eyedropper,  ///< Document pixel picker.
};

/// Display label and single-key shortcut for a tool. `key` is the binding
/// design-tool users expect (Selection = V, Pen = P, Type = T); `label` is the tool's
/// human name. The toolbar renders the tooltip as `"<label> (<key>)"`.
struct ToolKeybinding {
  std::string_view label;  //!< User-visible shortcut label.
  char key = '\0';         //!< Single-character keyboard binding, or zero when none is assigned.
};

/// The full toolbar tool list in display (top-to-bottom) order. The single
/// source of truth shared by the toolbar buttons, the tooltip text, and the
/// keyboard shortcut handler.
inline constexpr std::array<ToolId, 4> kToolbarTools = {
    ToolId::Select,
    ToolId::Pen,
    ToolId::Text,
    ToolId::Eyedropper,
};

/// Map a tool to its display label and standard shortcut key.
[[nodiscard]] constexpr ToolKeybinding KeybindingForTool(ToolId tool) {
  switch (tool) {
    case ToolId::Select: return ToolKeybinding{"Selection", 'V'};
    case ToolId::Pen: return ToolKeybinding{"Pen", 'P'};
    case ToolId::Text: return ToolKeybinding{"Type", 'T'};
    case ToolId::Eyedropper: return ToolKeybinding{"Eyedropper", 'I'};
  }
  return ToolKeybinding{"", '\0'};
}

}  // namespace donner::editor
