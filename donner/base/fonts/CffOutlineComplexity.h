#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace donner::fonts {

/// Bounded outline cost proved for one CFF or CFF2 glyph.
struct CffGlyphOutlineComplexity {
  uint32_t maximumVertices = 0;
  uint32_t work = 0;
};

/// Outcome of bounded CFF outline validation.
enum class CffOutlineValidationStatus : uint8_t {
  Complete,
  UnsupportedVariation,
  Invalid,
};

/// Per-glyph complexities returned by the bounded CFF interpreter.
struct CffOutlineValidationResult {
  CffOutlineValidationStatus status = CffOutlineValidationStatus::Invalid;
  std::vector<CffGlyphOutlineComplexity> glyphs;
};

/**
 * Validate CFF1 or non-variable CFF2 charstrings without materializing outlines.
 *
 * Variable CFF2 operators return \ref CffOutlineValidationStatus::UnsupportedVariation so callers
 * can retain directory validation while failing closed before an untrusted outline decoder.
 */
CffOutlineValidationResult ValidateCffOutlineComplexities(std::span<const uint8_t> table, bool cff2,
                                                          std::size_t expectedGlyphs);

}  // namespace donner::fonts
