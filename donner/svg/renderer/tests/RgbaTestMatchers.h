#pragma once
/// @file

#include <gmock/gmock.h>

#include <array>
#include <cstdint>
#include <string>

namespace donner::svg::test {

/// Render an RGBA pixel as "{r, g, b, a}" for matcher messages.
inline std::string FormatRgba(const std::array<uint8_t, 4>& px) {
  return testing::PrintToString(std::array<int, 4>{px[0], px[1], px[2], px[3]});
}

/// Matches a pixel whose channels each satisfy their own sub-matcher.
///
/// Prefer this over four separate channel expectations: a mismatch on any channel prints all four
/// channels at once and names the failing channel.
MATCHER_P4(Rgba, rMatcher, gMatcher, bMatcher, aMatcher,
           std::string("pixel RGBA matches {R=") + testing::DescribeMatcher<int>(rMatcher) +
               ", G=" + testing::DescribeMatcher<int>(gMatcher) +
               ", B=" + testing::DescribeMatcher<int>(bMatcher) +
               ", A=" + testing::DescribeMatcher<int>(aMatcher) + "}") {
  const std::array<uint8_t, 4> px = {arg[0], arg[1], arg[2], arg[3]};
  const testing::Matcher<int> channelMatchers[4] = {
      testing::SafeMatcherCast<int>(rMatcher), testing::SafeMatcherCast<int>(gMatcher),
      testing::SafeMatcherCast<int>(bMatcher), testing::SafeMatcherCast<int>(aMatcher)};
  static constexpr const char* kNames[4] = {"R", "G", "B", "A"};
  bool ok = true;
  for (int c = 0; c < 4; ++c) {
    if (!channelMatchers[c].Matches(px[c])) {
      ok = false;
    }
  }

  *result_listener << "actual RGBA=" << FormatRgba(px);
  if (!ok) {
    for (int c = 0; c < 4; ++c) {
      if (!channelMatchers[c].Matches(px[c])) {
        *result_listener << "; " << kNames[c] << "=" << static_cast<int>(px[c]) << " fails ("
                         << testing::DescribeMatcher<int>(channelMatchers[c]) << ")";
      }
    }
  }

  return ok;
}

/// Matches a pixel whose channels exactly equal the given RGBA values.
inline auto RgbaEq(int r, int g, int b, int a) {
  using testing::Eq;
  return Rgba(Eq(r), Eq(g), Eq(b), Eq(a));
}

/// Matches an integer channel within `tol` of `expected`.
inline testing::Matcher<int> Near(int expected, int tol) {
  return testing::AllOf(testing::Ge(expected - tol), testing::Le(expected + tol));
}

/// Matches a pixel whose alpha channel satisfies `alphaMatcher`, ignoring RGB.
MATCHER_P(Alpha, alphaMatcher, "alpha " + testing::DescribeMatcher<int>(alphaMatcher)) {
  const std::array<uint8_t, 4> px = {arg[0], arg[1], arg[2], arg[3]};
  *result_listener << "actual RGBA=" << FormatRgba(px);
  return testing::SafeMatcherCast<int>(alphaMatcher).Matches(static_cast<int>(px[3]));
}

/// Matches a fully transparent pixel (alpha == 0).
inline testing::Matcher<std::array<uint8_t, 4>> IsTransparent() {
  return Alpha(testing::Eq(0));
}

}  // namespace donner::svg::test
