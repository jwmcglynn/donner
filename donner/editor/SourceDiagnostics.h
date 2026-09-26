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
  DiagnosticSeverity severity =
      DiagnosticSeverity::Error;  //!< Diagnostic level used for display and counting.
  SourceByteRange range;          //!< Affected byte range in the source.
  std::size_t line = 1;           ///< One-based line number.
  std::size_t column = 0;         ///< Zero-based byte column.
  std::size_t endLine = 1;        ///< One-based line number for the exclusive range end.
  std::size_t endColumn = 0;      ///< Zero-based byte column for the exclusive range end.
  std::string message;            //!< Human-readable diagnostic text.

  /// Compare all members for value equality.
  /// @param other Value to compare.
  bool operator==(const SourceDiagnostic& other) const = default;
};

/// Immutable diagnostics published by one parse revision.
struct SourceDiagnosticSnapshot {
  std::uint64_t revision = 0;  //!< Source revision against which these diagnostics were produced.
  std::vector<SourceDiagnostic> diagnostics;  //!< Diagnostics for the captured source revision.

  /// Compare all members for value equality.
  /// @param other Value to compare.
  bool operator==(const SourceDiagnosticSnapshot& other) const = default;
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
