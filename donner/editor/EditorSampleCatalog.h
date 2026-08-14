#pragma once
/// @file

#include <span>
#include <string_view>

namespace donner::editor {

/// One built-in SVG document that can be loaded by editor UI surfaces.
///
/// The catalog owns the backing strings for the process lifetime. Sources may
/// be compiled assets or generated from them on first catalog access, so
/// callers can retain a view for as long as the process is running.
struct EditorSample {
  std::string_view id;      ///< Stable ASCII identifier used by UI state and commands.
  std::string_view title;   ///< Human-readable sample-picker label.
  std::string_view source;  ///< Complete SVG document source.
};

/// Return the bounded, ordered set of built-in editor samples.
[[nodiscard]] std::span<const EditorSample> GetEditorSampleCatalog() noexcept;

/// Find a built-in sample by its stable ASCII ID, or return nullptr.
[[nodiscard]] const EditorSample* FindEditorSample(std::string_view id) noexcept;

}  // namespace donner::editor
