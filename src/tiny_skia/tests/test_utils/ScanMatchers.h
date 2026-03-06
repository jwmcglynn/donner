#pragma once

#include <gmock/gmock.h>

#include <cstdint>

namespace tiny_skia::tests::matchers {

template <typename SpanT>
inline ::testing::Matcher<const SpanT&> SpanXYWidthEq(std::uint32_t x, std::uint32_t y,
                                                      std::uint32_t width) {
  using ::testing::AllOf;
  using ::testing::Eq;
  using ::testing::Field;

  return AllOf(Field("x", &SpanT::x, Eq(x)), Field("y", &SpanT::y, Eq(y)),
               Field("width", &SpanT::width, Eq(width)));
}

template <typename SpanT>
inline ::testing::Matcher<const SpanT&> SpanWidthEq(std::uint32_t width) {
  using ::testing::Eq;
  using ::testing::Field;

  return Field("width", &SpanT::width, Eq(width));
}

}  // namespace tiny_skia::tests::matchers
