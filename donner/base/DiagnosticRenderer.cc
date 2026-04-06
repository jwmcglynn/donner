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
  static constexpr const char* kBold = "\033[1m";
  static constexpr const char* kRed = "\033[31m";
  static constexpr const char* kYellow = "\033[33m";
  static constexpr const char* kCyan = "\033[36m";
  static constexpr const char* kBoldRed = "\033[1;31m";
  static constexpr const char* kBoldYellow = "\033[1;33m";
  static constexpr const char* kBoldCyan = "\033[1;36m";
  static constexpr const char* kBoldWhite = "\033[1;37m";
  static constexpr const char* kBlue = "\033[34m";
  static constexpr const char* kBoldBlue = "\033[1;34m";
};

/// Get the line containing the given offset in the source text.
/// Returns {line_start_offset, line_content}.
std::pair<size_t, std::string_view> getLineContent(std::string_view source, size_t offset) {
  // Find the start of the line.
  size_t lineStart = offset;
  while (lineStart > 0 && source[lineStart - 1] != '\n') {
    --lineStart;
  }

  // Find the end of the line.
  size_t lineEnd = offset;
  while (lineEnd < source.size() && source[lineEnd] != '\n') {
    ++lineEnd;
  }

  return {lineStart, source.substr(lineStart, lineEnd - lineStart)};
}

/// Format the width of a line number for alignment.
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
  std::ostringstream out;

  const bool color = options.colorize;

  // Severity label.
  if (diag.severity == DiagnosticSeverity::Error) {
    if (color) {
      out << AnsiColors::kBoldRed;
    }
    out << "error";
  } else {
    if (color) {
      out << AnsiColors::kBoldYellow;
    }
    out << "warning";
  }
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

  // Clamp startOffset to source bounds.
  if (startOffset > source.size()) {
    return out.str();
  }

  // Compute line info.
  parser::LineOffsets lineOffsets(source);
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

  // Get the source line.
  auto [lineStart, lineContent] = getLineContent(source, startOffset);

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

  // Spaces to reach the column.
  out << std::string(column, ' ');

  // Compute span width.
  size_t spanWidth = 1;  // Default: single caret.
  if (diag.range.end.offset.has_value()) {
    const size_t endOffset = diag.range.end.offset.value();
    if (endOffset > startOffset) {
      // Clamp to same line.
      const size_t lineEndOffset = lineStart + lineContent.size();
      const size_t clampedEnd = std::min(endOffset, lineEndOffset);
      if (clampedEnd > startOffset) {
        spanWidth = clampedEnd - startOffset;
      }
    }
  }

  // Draw caret + tildes.
  if (color) {
    if (diag.severity == DiagnosticSeverity::Error) {
      out << AnsiColors::kBoldRed;
    } else {
      out << AnsiColors::kBoldYellow;
    }
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
  std::string result;
  for (const auto& diag : sink.warnings()) {
    result += format(source, diag, options);
  }
  return result;
}

}  // namespace donner
