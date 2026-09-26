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
  uint32_t maximumVertices = 0;  //!< Conservative upper bound on vertices produced by this glyph.
  uint32_t work = 0;             //!< Validation work charged while analyzing this glyph.
};

/// Outcome of bounded CFF outline validation.
enum class CffOutlineValidationStatus : uint8_t {
  Complete,              ///< Every glyph validated within the work budget.
  UnsupportedVariation,  ///< Variable CFF2 operators require a decoder this validator cannot bound.
  WorkLimitExceeded,     ///< Validation exhausted the caller's work budget.
  Invalid,               ///< The CFF data is malformed.
};

/// Per-glyph complexities returned by the bounded CFF interpreter.
struct CffOutlineValidationResult {
  CffOutlineValidationStatus status =
      CffOutlineValidationStatus::Invalid;  //!< Overall CFF validation outcome.

  /// Structure and CharString work actually consumed, including failed validation.
  std::size_t work = 0;
  /// Work spent resolving legacy CFF1 endchar component graphs.
  std::size_t componentResolutionWork = 0;
  std::vector<CffGlyphOutlineComplexity>
      glyphs;  //!< Per-glyph outline complexity bounds for the validated font.
};

/**
 * Validate CFF1 or non-variable CFF2 charstrings without materializing outlines.
 *
 * Variable CFF2 operators return \ref
 * donner::fonts::CffOutlineValidationStatus::UnsupportedVariation so callers can retain directory
 * validation while failing closed before an untrusted outline decoder. Legacy CFF1 endchar
 * composites include their resolved component costs in the returned bound.
 *
 * @param table Exact CFF or CFF2 table bytes.
 * @param cff2 Whether @p table uses CFF2 structures and CharStrings.
 * @param expectedGlyphs Glyph count from the sfnt `maxp` table.
 * @param maximumWork Caller-owned work remaining for this validation.
 */
CffOutlineValidationResult ValidateCffOutlineComplexities(
    std::span<const uint8_t> table, bool cff2, std::size_t expectedGlyphs,
    std::size_t maximumWork = kMaximumCffOutlineValidationWork);

}  // namespace donner::fonts
