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
  std::size_t errors = 0;
  std::size_t warnings = 0;

  bool operator==(const SourceDiagnosticCounts&) const = default;
};

[[nodiscard]] SourceDiagnosticCounts CountSourceDiagnostics(
    std::span<const SourceDiagnostic> diagnostics);

[[nodiscard]] const SourceDiagnostic* FindSourceDiagnostic(
    std::span<const SourceDiagnostic> diagnostics, std::uint64_t id);

/// User interaction emitted by one diagnostics panel frame.
struct SourceDiagnosticsPanelAction {
  std::optional<std::uint64_t> hoveredId;
  std::optional<std::uint64_t> activatedId;
};

/// Compact bottom panel presenting source warnings and errors.
class SourceDiagnosticsPanel {
public:
  [[nodiscard]] SourceDiagnosticsPanelAction render(std::span<const SourceDiagnostic> diagnostics,
                                                    std::optional<std::uint64_t> sourceHoveredId,
                                                    float height);
};

}  // namespace donner::editor
