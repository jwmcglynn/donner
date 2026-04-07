#pragma once
/// @file

#include <ostream>

#include "donner/base/FileOffset.h"
#include "donner/base/RcString.h"

namespace donner {

/// Severity level for a parser diagnostic.
enum class DiagnosticSeverity : uint8_t {
  Warning,  ///< Non-fatal issue; parsing continues.
  Error,    ///< Fatal issue; parsing may stop or produce partial results.
};

/// Ostream output operator for \ref DiagnosticSeverity.
inline std::ostream& operator<<(std::ostream& os, DiagnosticSeverity severity) {
  switch (severity) {
    case DiagnosticSeverity::Warning: return os << "warning";
    case DiagnosticSeverity::Error: return os << "error";
  }
  return os << "unknown";
}

/**
 * A diagnostic message from a parser, with severity, source range, and human-readable reason.
 *
 * This is the shared diagnostic type used across all donner parsers (XML, SVG, CSS, etc.).
 */
struct ParseDiagnostic {
  /// Severity of this diagnostic.
  DiagnosticSeverity severity = DiagnosticSeverity::Error;

  /// Human-readable description of the problem.
  RcString reason;

  /// Source range that this diagnostic applies to. For point diagnostics where
  /// the end is unknown, start == end.
  SourceRange range = {FileOffset::Offset(0), FileOffset::Offset(0)};

  /// Create an error diagnostic at a single offset.
  static ParseDiagnostic Error(RcString reason, FileOffset location) {
    return {DiagnosticSeverity::Error, std::move(reason), {location, location}};
  }

  /// Create an error diagnostic with a source range.
  static ParseDiagnostic Error(RcString reason, SourceRange range) {
    return {DiagnosticSeverity::Error, std::move(reason), range};
  }

  /// Create a warning diagnostic at a single offset.
  static ParseDiagnostic Warning(RcString reason, FileOffset location) {
    return {DiagnosticSeverity::Warning, std::move(reason), {location, location}};
  }

  /// Create a warning diagnostic with a source range.
  static ParseDiagnostic Warning(RcString reason, SourceRange range) {
    return {DiagnosticSeverity::Warning, std::move(reason), range};
  }

  /// Ostream output operator for \ref ParseDiagnostic, outputs severity and error message.
  friend std::ostream& operator<<(std::ostream& os, const ParseDiagnostic& diag);
};

}  // namespace donner
