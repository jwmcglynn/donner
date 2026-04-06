#include "donner/base/DiagnosticRenderer.h"

#include <algorithm>
#include <sstream>
#include <string>

#include "donner/base/parser/LineOffsets.h"

namespace donner {

namespace {

/// ANSI escape codes for colorized output.
struct AnsiColors {
  static constexpr const char* kReset = "\033[0m";
  static constexpr const char* kBoldRed = "\033[1;31m";
  static constexpr const char* kBoldYellow = "\033[1;33m";
  static constexpr const char* kBoldWhite = "\033[1;37m";
  static constexpr const char* kBoldBlue = "\033[1;34m";
};

/// Get the content of the line at the given line number (1-indexed), using LineOffsets for proper
/// newline handling (\n, \r, \r\n).
std::string_view getLineContent(std::string_view source, const parser::LineOffsets& lineOffsets,
                                size_t line) {
  const size_t lineStart = lineOffsets.lineOffset(line);

  // Find the end of the line — look for any newline character.
  size_t lineEnd = lineStart;
  while (lineEnd < source.size() && source[lineEnd] != '\n' && source[lineEnd] != '\r') {
    ++lineEnd;
  }

  return source.substr(lineStart, lineEnd - lineStart);
}

/// Compute the number of decimal digits needed to display a line number.
int lineNumberWidth(size_t lineNumber) {
  int width = 1;
  while (lineNumber >= 10) {
    lineNumber /= 10;
    ++width;
  }
  return width;
}

}  // namespace

std::string DiagnosticRenderer::format(std::string_view source, const ParseDiagnostic& diag,
                                       const Options& options) {
  parser::LineOffsets lineOffsets(source);
  return formatWithLineOffsets(source, lineOffsets, diag, options);
}

std::string DiagnosticRenderer::formatWithLineOffsets(std::string_view source,
                                                      const parser::LineOffsets& lineOffsets,
                                                      const ParseDiagnostic& diag,
                                                      const Options& options) {
  std::ostringstream out;

  const bool color = options.colorize;

  // Severity label.
  if (color) {
    out << (diag.severity == DiagnosticSeverity::Error ? AnsiColors::kBoldRed
                                                       : AnsiColors::kBoldYellow);
  }
  out << (diag.severity == DiagnosticSeverity::Error ? "error" : "warning");
  if (color) {
    out << AnsiColors::kBoldWhite;
  }
  out << ": " << diag.reason;
  if (color) {
    out << AnsiColors::kReset;
  }
  out << "\n";

  // If the range has no offset (EndOfString), we can only show the message.
  if (!diag.range.start.offset.has_value()) {
    return out.str();
  }

  const size_t startOffset = diag.range.start.offset.value();

  if (startOffset > source.size()) {
    return out.str();
  }

  const size_t line = lineOffsets.offsetToLine(startOffset);
  const size_t lineStartOffset = lineOffsets.lineOffset(line);
  const size_t column = startOffset - lineStartOffset;

  // Location header.
  if (color) {
    out << AnsiColors::kBoldBlue;
  }
  out << "  --> ";
  if (color) {
    out << AnsiColors::kReset;
  }
  if (!options.filename.empty()) {
    out << options.filename << ":";
  }
  out << line << ":" << (column + 1) << "\n";

  const std::string_view lineContent = getLineContent(source, lineOffsets, line);
  const int numWidth = lineNumberWidth(line);

  // Empty gutter line.
  if (color) {
    out << AnsiColors::kBoldBlue;
  }
  out << std::string(numWidth + 1, ' ') << "|";
  if (color) {
    out << AnsiColors::kReset;
  }
  out << "\n";

  // Source line.
  if (color) {
    out << AnsiColors::kBoldBlue;
  }
  out << " " << line << " | ";
  if (color) {
    out << AnsiColors::kReset;
  }
  out << lineContent << "\n";

  // Caret/tilde indicator line.
  if (color) {
    out << AnsiColors::kBoldBlue;
  }
  out << std::string(numWidth + 1, ' ') << "| ";
  if (color) {
    out << AnsiColors::kReset;
  }

  out << std::string(column, ' ');

  // Compute span width, clamped to the current line.
  size_t spanWidth = 1;
  if (diag.range.end.offset.has_value()) {
    const size_t endOffset = diag.range.end.offset.value();
    if (endOffset > startOffset) {
      const size_t lineEndOffset = lineStartOffset + lineContent.size();
      const size_t clampedEnd = std::min(endOffset, lineEndOffset);
      if (clampedEnd > startOffset) {
        spanWidth = clampedEnd - startOffset;
      }
    }
  }

  // Draw caret + tildes.
  if (color) {
    out << (diag.severity == DiagnosticSeverity::Error ? AnsiColors::kBoldRed
                                                       : AnsiColors::kBoldYellow);
  }
  out << "^";
  if (spanWidth > 1) {
    out << std::string(spanWidth - 1, '~');
  }
  if (color) {
    out << AnsiColors::kReset;
  }
  out << "\n";

  return out.str();
}

std::string DiagnosticRenderer::formatAll(std::string_view source, const ParseWarningSink& sink,
                                          const Options& options) {
  parser::LineOffsets lineOffsets(source);
  std::string result;
  for (const auto& diag : sink.warnings()) {
    result += formatWithLineOffsets(source, lineOffsets, diag, options);
  }
  return result;
}

}  // namespace donner
