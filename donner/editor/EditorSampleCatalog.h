#pragma once
/// @file

#include <span>
#include <string_view>

namespace donner::editor {

/// One built-in SVG document that can be loaded by editor UI surfaces.
///
/// The catalog owns none of the strings. Each view refers to static compiled
/// data, so callers can retain a view for as long as the process is running.
struct EditorSample {
  std::string_view id;
  std::string_view title;
  std::string_view source;
};

/// Return the bounded, ordered set of built-in editor samples.
[[nodiscard]] std::span<const EditorSample> GetEditorSampleCatalog() noexcept;

/// Find a built-in sample by its stable ASCII ID, or return nullptr.
[[nodiscard]] const EditorSample* FindEditorSample(std::string_view id) noexcept;

}  // namespace donner::editor
