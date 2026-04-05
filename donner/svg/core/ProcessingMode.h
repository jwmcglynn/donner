#pragma once
/// @file

#include <cstdint>

namespace donner::svg {

/**
 * SVG document processing mode, per SVG2 §2.7.1.
 *
 * Controls which features are available when processing a document. Sub-documents referenced by
 * `<image>` are loaded in \ref SecureStatic or \ref SecureAnimated mode to prevent external
 * resource loading and script execution.
 */
enum class ProcessingMode : uint8_t {
  DynamicInteractive,  ///< Full features enabled (default for top-level documents).
  SecureAnimated,      ///< No scripts, no external refs; animations allowed.
  SecureStatic,        ///< No scripts, no external refs, no animations.
};

}  // namespace donner::svg
