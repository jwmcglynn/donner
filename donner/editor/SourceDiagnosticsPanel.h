#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "donner/editor/SourceDiagnostics.h"

namespace donner::editor {

/// Severity totals shown in the source diagnostics header.
struct SourceDiagnosticCounts {
  std::size_t errors = 0;    //!< Number of error diagnostics.
  std::size_t warnings = 0;  //!< Number of warning diagnostics.

  /// Compare all members for value equality.
  /// @param other Value to compare.
  bool operator==(const SourceDiagnosticCounts& other) const = default;
};

/// Count errors and warnings in a diagnostic snapshot.
/// @param diagnostics Diagnostics to count.
[[nodiscard]] SourceDiagnosticCounts CountSourceDiagnostics(
    std::span<const SourceDiagnostic> diagnostics);

/// Find a diagnostic by stable identifier; return null when absent.
/// @param diagnostics Borrowed diagnostics to search.
/// @param id Identifier to find.
[[nodiscard]] const SourceDiagnostic* FindSourceDiagnostic(
    std::span<const SourceDiagnostic> diagnostics, std::uint64_t id);

/// User interaction emitted by one diagnostics panel frame.
struct SourceDiagnosticsPanelAction {
  std::optional<std::uint64_t> hoveredId;  //!< Diagnostic identifier under the pointer, when any.
  std::optional<std::uint64_t>
      activatedId;  //!< Diagnostic identifier activated for source navigation, when any.
};

/// Compact bottom panel presenting source warnings and errors.
class SourceDiagnosticsPanel {
public:
  /// Draw the diagnostic list and report hover or activation actions.
  /// @param diagnostics Diagnostics for the displayed source revision.
  /// @param sourceHoveredId Optional diagnostic highlighted by source-pane hover.
  /// @param height Available panel height in logical UI pixels.
  [[nodiscard]] SourceDiagnosticsPanelAction render(std::span<const SourceDiagnostic> diagnostics,
                                                    std::optional<std::uint64_t> sourceHoveredId,
                                                    float height);
};

}  // namespace donner::editor
