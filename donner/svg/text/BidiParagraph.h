#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "donner/svg/text/BidiControl.h"

namespace donner::svg {

/// Cumulative admission for one text layout, including fast paths and all source spans.
struct BidiResourceBudget {
  /// Limits independently bound input, visual complexity and placed geometry.
  struct Limits {
    size_t codepoints = 1000000;
    size_t runs = 65536;
    size_t runCandidates = 8000000;
    size_t ownerWork = 8000000;
    size_t paragraphs = 65536;
    size_t glyphs = 1000000;
  };
  Limits remaining;
  bool rejected = false;

  /// Charge a quantity without overflow; a refusal rejects all subsequent work.
  bool consume(size_t& available, size_t count) {
    if (rejected || count > available) {
      rejected = true;
      return false;
    }
    available -= count;
    return true;
  }
};

/// Owns stable Unicode scalar storage and resolved paragraphs. No library handle escapes.
class BidiParagraph {
public:
  /// One level run of a line, returned in left-to-right visual order.
  struct Run {
    uint32_t start = 0;
    uint32_t end = 0;
    uint8_t level = 0;
  };

  /// A control that an inline element's `unicode-bidi` places around its content. It takes part in
  /// analysis but occupies no scalar index, so levels and runs describe only the scalars.
  struct InjectedControl {
    uint32_t before = 0;     //!< Scalar index the control precedes; the scalar count for the end.
    char32_t codepoint = 0;  //!< An explicit embedding, override, isolate or pop control.
  };

  /// Analyze Donner-decoded scalars. Base 0/1 is explicit; 2 requests first-strong detection.
  /// @param codepoints Owned scalars, which remain pinned throughout analysis and line queries.
  /// @param budget Cumulative admission shared by the caller's layout.
  /// @param base Paragraph base direction: 0 for `direction: ltr`, 1 for `rtl`.
  /// @param forceAnalysis Whether to bypass the equivalent all-left-to-right fast path.
  /// @param override Whether every scalar takes the explicit base level without reordering by
  ///   bidi type, as `unicode-bidi: bidi-override` on the paragraph's block container. Requires
  ///   base 0 or 1.
  /// @param controls Injected controls in nondecreasing \ref InjectedControl::before order; each
  ///   counts against the scalar budget. Controls nested inside an override still open levels.
  static std::optional<BidiParagraph> Analyze(std::vector<char32_t> codepoints,
                                              BidiResourceBudget& budget, uint8_t base = 0,
                                              bool forceAnalysis = false, bool override = false,
                                              std::span<const InjectedControl> controls = {});
  ~BidiParagraph();
  BidiParagraph(BidiParagraph&&) noexcept;
  BidiParagraph& operator=(BidiParagraph&&) noexcept;
  BidiParagraph(const BidiParagraph&) = delete;
  BidiParagraph& operator=(const BidiParagraph&) = delete;

  /// Resolve L1/L2 for a logical line; source paragraphs never reorder across each other.
  /// @param start First scalar of the line.
  /// @param end Exclusive scalar end.
  /// @param budget Shared visual-run admission.
  std::optional<std::vector<Run>> visualRuns(uint32_t start, uint32_t end,
                                             BidiResourceBudget& budget) const;
  /// Resolved level before line-specific trailing-space resets.
  uint8_t levelAt(uint32_t index) const;
  /// Resolved base level of the source paragraph containing the scalar.
  uint8_t paragraphLevelAt(uint32_t index) const;
  /// Whether a scalar can require right-to-left analysis at explicit base zero.
  static bool NeedsAnalysis(char32_t codepoint);
  /// Whether a source character affects bidi but consumes no SVG or DOM character index.
  static bool IsControl(char32_t codepoint) { return IsBidiControlCodepoint(codepoint); }
  /// Whether the scalar is a nonspacing mark that stays attached to its preceding base.
  static bool IsNonspacing(char32_t codepoint);
  /// Unicode L4 mirror; characters without a paired mirror remain unchanged.
  static char32_t Mirror(char32_t codepoint);

private:
  /// Number of scalars, excluding injected controls.
  uint32_t scalarCount() const;

  struct State;
  explicit BidiParagraph(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

static_assert(!std::is_copy_constructible_v<BidiParagraph>);
static_assert(std::is_nothrow_move_constructible_v<BidiParagraph>);

}  // namespace donner::svg
