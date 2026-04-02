#pragma once

#include <gmock/gmock.h>

#include <cstdint>
#include <optional>

#include "tiny_skia/Geom.h"

namespace tiny_skia::tests::matchers {

inline ::testing::Matcher<const tiny_skia::ScreenIntRect&> ScreenIntRectEq(std::uint32_t x,
                                                                           std::uint32_t y,
                                                                           std::uint32_t width,
                                                                           std::uint32_t height) {
  using ::testing::AllOf;
  using ::testing::Eq;
  using ::testing::Property;

  return AllOf(Property("x", &tiny_skia::ScreenIntRect::x, Eq(x)),
               Property("y", &tiny_skia::ScreenIntRect::y, Eq(y)),
               Property("width", &tiny_skia::ScreenIntRect::width, Eq(width)),
               Property("height", &tiny_skia::ScreenIntRect::height, Eq(height)));
}

inline ::testing::Matcher<const std::optional<tiny_skia::ScreenIntRect>&> OptionalScreenIntRectEq(
    std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) {
  return ::testing::Optional(ScreenIntRectEq(x, y, width, height));
}

}  // namespace tiny_skia::tests::matchers
