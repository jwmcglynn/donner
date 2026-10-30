#pragma once
/// @file

#include <cstdint>

namespace donner::svg {

/// Streaming Unicode 17.0.0 extended grapheme boundaries (UAX #29 revision 47).
/// State is constant size and may continue across adjacent text spans.
class GraphemeBreaker {
public:
  /// Consume one codepoint and report whether a grapheme starts before it.
  /// @param codepoint Next normalized text codepoint, in logical order.
  bool breakBefore(char32_t codepoint);

private:
  bool boundary(uint8_t next, uint8_t indic, bool pictographic) const;
  void advance(uint8_t next, uint8_t indic, bool pictographic);

  uint8_t previous_ = 0;
  uint8_t indic_ = 0;
  bool started_ = false;
  bool oddRegional_ = false;
  bool pictographicExtend_ = false;
  bool pictographicZwj_ = false;
};

/// Detect potential Unicode lam-alef groups; actual font shaping must qualify their boundaries.
class LamAlefCandidateBreaker {
public:
  /// Consume one codepoint, retaining transparent marks inside potential lam-alef groups.
  /// @param codepoint Next normalized text codepoint, in logical order.
  bool breakBefore(char32_t codepoint);

  /// Whether the most recently consumed codepoint completes a potential lam-alef sequence.
  bool completesPair() const { return pair_; }

private:
  GraphemeBreaker graphemes_;
  bool lam_ = false;
  bool pair_ = false;
};

}  // namespace donner::svg
