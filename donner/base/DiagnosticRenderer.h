#pragma once
/// @file

#include <string>
#include <string_view>

#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"

namespace donner {

/**
 * Renders diagnostic messages with source context and caret/tilde indicators, similar to
 * clang/rustc output.
 *
 * Example output:
 * ```
 * error: Unexpected character
 *   --> test.svg:1:25
 *    |
 *  1 | <path d="M 100 100 h 2!" />
 *    |                         ^
 * ```
 *
 * For multi-character ranges, tildes indicate the span:
 * ```
 * warning: Invalid paint server value
 *   --> 4:13
 *    |
 *  4 | <path fill="url(#)"/>
 *    |             ^~~~~~
 * ```
 */
class DiagnosticRenderer {
public:
  /**
   * Options for rendering diagnostics.
   */
  struct Options {
    /// Optional filename for the header (e.g. "test.svg").
    std::string_view filename;

    /// Enable ANSI color codes in the output.
    bool colorize = false;
  };

  /**
   * Format a single diagnostic against source text.
   *
   * The location is printed as a 1-based line and column. A range that spans several lines is
   * underlined only to the end of its first line. A diagnostic without a resolved offset, or with
   * an offset past the end of \p source, renders only the message line.
   *
   * @param source The original source text that was parsed.
   * @param diag The diagnostic to render.
   * @param options Rendering options.
   * @return Formatted diagnostic string with source context.
   */
  static std::string format(std::string_view source, const ParseDiagnostic& diag,
                            const Options& options);

  /// Format with default options.
  static std::string format(std::string_view source, const ParseDiagnostic& diag) {
    return format(source, diag, Options{});
  }

  /**
   * Format all warnings in a sink against source text.
   *
   * @param source The original source text that was parsed.
   * @param sink Warning sink containing diagnostics to render.
   * @param options Rendering options.
   * @return Formatted diagnostic strings, concatenated.
   */
  static std::string formatAll(std::string_view source, const ParseWarningSink& sink,
                               const Options& options);

  /// Format all with default options.
  static std::string formatAll(std::string_view source, const ParseWarningSink& sink) {
    return formatAll(source, sink, Options{});
  }
};

}  // namespace donner
