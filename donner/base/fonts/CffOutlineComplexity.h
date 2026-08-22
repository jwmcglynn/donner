#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace donner::fonts {

/// Default whole-font work ceiling for one bounded CFF validation.
inline constexpr std::size_t kMaximumCffOutlineValidationWork = 64 * 1024 * 1024;

/// Bounded outline cost proved for one CFF or CFF2 glyph.
struct CffGlyphOutlineComplexity {
  uint32_t maximumVertices = 0;
  uint32_t work = 0;
  bool renderable = true;
};

/// Outcome of bounded CFF outline validation.
enum class CffOutlineValidationStatus : uint8_t {
  Complete,
  UnsupportedVariation,
  WorkLimitExceeded,
  Invalid,
};

/// Per-glyph complexities returned by the bounded CFF interpreter.
struct CffOutlineValidationResult {
  CffOutlineValidationStatus status = CffOutlineValidationStatus::Invalid;
  std::size_t work = 0;
  std::vector<CffGlyphOutlineComplexity> glyphs;
};

/**
 * Validate CFF1 or non-variable CFF2 charstrings without materializing outlines.
 *
 * Variable CFF2 operators return \ref CffOutlineValidationStatus::UnsupportedVariation so callers
 * can retain directory validation while failing closed before an untrusted outline decoder.
 */
CffOutlineValidationResult ValidateCffOutlineComplexities(
    std::span<const uint8_t> table, bool cff2, std::size_t expectedGlyphs,
    std::size_t maximumWork = kMaximumCffOutlineValidationWork);

}  // namespace donner::fonts
