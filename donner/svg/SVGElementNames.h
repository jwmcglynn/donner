#pragma once
/// @file
///
/// Compile-time lists of SVG element tag names and known attribute names, derived from donner's
/// type registries. Used by the parser fuzzer, editor syntax highlighting, and autocomplete.

#include <array>
#include <string_view>

#include "donner/svg/AllSVGElements.h"

namespace donner::svg {

namespace detail {

/// Extract Tag constants from AllSVGElements type list at compile time.
template <typename... Types>
constexpr auto extractTagNames(entt::type_list<Types...>) {
  return std::array<std::string_view, sizeof...(Types)>{Types::Tag...};
}

}  // namespace detail

/// Compile-time array of all known SVG element tag names (e.g. "rect", "circle", "path").
/// Derived from AllSVGElements and each element class's static Tag constant.
inline constexpr auto kSVGElementNames = detail::extractTagNames(AllSVGElements{});

/// Compile-time array of all known SVG presentation attributes and geometry attributes.
/// Each entry is an attribute name string (e.g. "fill", "stroke", "transform").
inline constexpr std::array<std::string_view, 70> kSVGPresentationAttributeNames{{
    "cx",
    "cy",
    "height",
    "width",
    "x",
    "y",
    "r",
    "rx",
    "ry",
    "d",
    "fill",
    "transform",
    "alignment-baseline",
    "baseline-shift",
    "clip-path",
    "clip-rule",
    "color",
    "color-interpolation",
    "color-interpolation-filters",
    "color-rendering",
    "cursor",
    "direction",
    "display",
    "dominant-baseline",
    "fill-opacity",
    "fill-rule",
    "filter",
    "flood-color",
    "flood-opacity",
    "font-family",
    "font-size",
    "font-size-adjust",
    "font-stretch",
    "font-style",
    "font-variant",
    "font-weight",
    "glyph-orientation-horizontal",
    "glyph-orientation-vertical",
    "image-rendering",
    "letter-spacing",
    "lighting-color",
    "marker-end",
    "marker-mid",
    "marker-start",
    "mask",
    "opacity",
    "overflow",
    "paint-order",
    "pointer-events",
    "shape-rendering",
    "stop-color",
    "stop-opacity",
    "stroke",
    "stroke-dasharray",
    "stroke-dashoffset",
    "stroke-linecap",
    "stroke-linejoin",
    "stroke-miterlimit",
    "stroke-opacity",
    "stroke-width",
    "text-anchor",
    "text-decoration",
    "text-overflow",
    "text-rendering",
    "unicode-bidi",
    "vector-effect",
    "visibility",
    "white-space",
    "word-spacing",
    "writing-mode",
}};

}  // namespace donner::svg
