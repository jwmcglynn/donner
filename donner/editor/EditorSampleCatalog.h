#pragma once
/// @file

#include <span>
#include <string_view>

#include "donner/base/PackagedResource.h"

namespace donner::editor {

/// Lazy immutable sample source. Enumerating IDs and titles does not read payload bytes.
struct EditorSampleSource {
  const PackagedResource& resource;
  std::string_view view() const;
  operator std::string_view() const { return view(); }
  bool empty() const { return view().empty(); }
  /// Request bytes outside document access. Browser completion is polled by the owning frame.
  bool request() const;
};

/// One built-in SVG document that can be loaded by editor UI surfaces.
///
/// The application resource session owns backing bytes. Keep it alive while consuming sources.
struct EditorSample {
  std::string_view id;        ///< Stable ASCII identifier used by UI state and commands.
  std::string_view title;     ///< Human-readable sample-picker label.
  EditorSampleSource source;  ///< Deferred complete SVG document source.
};

/// Return the bounded, ordered set of built-in editor samples.
[[nodiscard]] std::span<const EditorSample> GetEditorSampleCatalog() noexcept;

/// Find a built-in sample by its stable ASCII ID, or return nullptr.
[[nodiscard]] const EditorSample* FindEditorSample(std::string_view id) noexcept;

}  // namespace donner::editor
