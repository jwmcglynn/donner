#include "donner/svg/text/GraphemeBreak.h"

#include <algorithm>
#include <span>

#include "donner/svg/text/GraphemeProperties.h"

namespace donner::svg {
namespace {

enum Property : uint8_t {
  Other,
  CR,
  LF,
  Control,
  Extend,
  ZWJ,
  Regional,
  Prepend,
  SpacingMark,
  L,
  V,
  T,
  LV,
  LVT
};

uint8_t Lookup(char32_t codepoint, std::span<const detail::GraphemePropertyRange> ranges) {
  const auto found =
      std::lower_bound(ranges.begin(), ranges.end(), codepoint,
                       [](const auto& range, char32_t value) { return range.last < value; });
  return found != ranges.end() && (found->firstAndValue & 0x1fffffu) <= codepoint
             ? static_cast<uint8_t>(found->firstAndValue >> 21)
             : 0;
}

uint8_t GraphemeProperty(char32_t codepoint) {
  if (codepoint >= 0xac00 && codepoint <= 0xd7a3) {
    return (codepoint - 0xac00) % 28 == 0 ? LV : LVT;
  }
  return Lookup(codepoint, detail::kGraphemeRanges);
}

bool IsControl(uint8_t property) {
  return property == CR || property == LF || property == Control;
}

bool HangulJoins(uint8_t previous, uint8_t next) {
  if (previous == L) {
    return next == L || next == V || next == LV || next == LVT;
  }
  if (previous == LV || previous == V) {
    return next == V || next == T;
  }
  return (previous == LVT || previous == T) && next == T;
}

bool BasicBoundary(uint8_t previous, uint8_t next) {
  if (previous == CR && next == LF) {
    return false;
  }
  if (IsControl(previous) || IsControl(next)) {
    return true;
  }
  return !HangulJoins(previous, next) && next != Extend && next != ZWJ && next != SpacingMark &&
         previous != Prepend;
}

}  // namespace

bool GraphemeBreaker::breakBefore(char32_t codepoint) {
  const uint8_t next = GraphemeProperty(codepoint);
  const uint8_t indic = Lookup(codepoint, detail::kIndicRanges);
  const bool pictographic = Lookup(codepoint, detail::kEmojiRanges) != 0;
  const bool result = !started_ || boundary(next, indic, pictographic);
  advance(next, indic, pictographic);
  return result;
}

bool GraphemeBreaker::boundary(uint8_t next, uint8_t indic, bool pictographic) const {
  return BasicBoundary(previous_, next) && !(indic_ == 2 && indic == 1) &&
         !(pictographicZwj_ && pictographic) &&
         !(previous_ == Regional && next == Regional && oddRegional_);
}

void GraphemeBreaker::advance(uint8_t next, uint8_t indic, bool pictographic) {
  pictographicZwj_ = next == ZWJ && pictographicExtend_;
  pictographicExtend_ = pictographic || (next == Extend && pictographicExtend_);
  if (indic == 1) {
    indic_ = 1;
  } else if (indic == 3 && indic_ != 0) {
    indic_ = 2;
  } else if (indic != 2) {
    indic_ = 0;
  }
  oddRegional_ = next == Regional && !oddRegional_;
  previous_ = next;
  started_ = true;
}

bool LamAlefCandidateBreaker::breakBefore(char32_t codepoint) {
  const bool grapheme = graphemes_.breakBefore(codepoint);
  const uint8_t group = Lookup(codepoint, detail::kArabicRanges);
  const uint8_t joiningOverride = Lookup(codepoint, detail::kJoiningRanges);
  const bool transparent =
      joiningOverride == 1 || (joiningOverride == 0 && GraphemeProperty(codepoint) == Extend);
  pair_ = lam_ && group == 2;
  const bool joined = lam_ && (transparent || group == 2);
  lam_ = transparent ? lam_ : group == 1;
  return grapheme && !joined;
}

}  // namespace donner::svg
