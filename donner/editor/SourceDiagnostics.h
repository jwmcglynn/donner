#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseDiagnostic.h"
#include "donner/editor/FlashDecorations.h"

namespace donner::editor {

/// Source diagnostic prepared for editor presentation.
struct SourceDiagnostic {
  std::uint64_t id = 0;  ///< Stable within one parse revision.
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  SourceByteRange range;
  std::size_t line = 1;    ///< One-based line number.
  std::size_t column = 0;  ///< Zero-based byte column.
  std::string message;

  bool operator==(const SourceDiagnostic&) const = default;
};

/// Immutable diagnostics published by one parse revision.
struct SourceDiagnosticSnapshot {
  std::uint64_t revision = 0;
  std::vector<SourceDiagnostic> diagnostics;

  bool operator==(const SourceDiagnosticSnapshot&) const = default;
};

/**
 * Normalize parser diagnostics for source-editor presentation.
 *
 * Ranges are clamped to \p source, point diagnostics expand to a visible byte when possible, and
 * missing line information is recovered from the source text. Diagnostic ids are deterministic
 * for the same revision and change when a new parse revision publishes the same diagnostic.
 */
[[nodiscard]] SourceDiagnosticSnapshot BuildSourceDiagnosticSnapshot(
    std::span<const ParseDiagnostic> diagnostics, std::string_view source, std::uint64_t revision);

}  // namespace donner::editor
