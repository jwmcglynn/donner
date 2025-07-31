

#pragma once
/// @file

#include <string_view>

namespace donner::svg {

/// The user agent stylesheet to be used by default on all SVG documents.
/// @see https://www.w3.org/TR/SVG2/styling.html#UAStyleSheet
constexpr std::string_view kUserAgentStylesheet = R"(
@namespace url(http://www.w3.org/2000/svg);
@namespace xml url(http://www.w3.org/XML/1998/namespace);

svg:not(:root), image, marker, pattern, symbol { overflow: hidden; }

*:not(svg),
*:not(foreignObject) > svg {
  transform-origin: 0 0;
}

*[xml|space=preserve] {
  text-space-collapse: preserve-spaces;
}

:host(use) > symbol {
  display: inline !important;
}
:link, :visited {
  cursor: pointer;
}
)";

}  // namespace donner::svg
