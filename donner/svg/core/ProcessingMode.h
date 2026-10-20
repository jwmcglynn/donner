#pragma once
/// @file

#include <cstdint>

namespace donner::svg {

/**
 * SVG document processing mode, per SVG2 §2.7.1.
 *
 * Controls which features are available when processing a document. Select the mode of a
 * top-level document with \ref donner::svg::SVGDocument::Settings::processingMode when parsing.
 * Unless the host supplies its own \ref donner::svg::SVGDocument::Settings::svgParseCallback,
 * sub-documents referenced by `<image>` are parsed in \ref ProcessingMode::SecureStatic mode, so
 * they neither load external resources nor animate. Donner never executes scripts in any
 * mode.
 */
enum class ProcessingMode : uint8_t {
  /// Full features enabled (default for top-level documents), including external resources
  /// through the document's resource loader, animation, host event dispatch and host-scheduled
  /// animation begin and end.
  DynamicInteractive,
  /// No external resources. Authored animation timing runs, but host event dispatch and
  /// host-scheduled animation begin and end are rejected.
  SecureAnimated,
  /// No external resources and no animation: animated values never apply.
  SecureStatic,
};

}  // namespace donner::svg
