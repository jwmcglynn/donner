#pragma once
/// @file
/// @brief Shared representative inputs for CSS parser benchmarks.

#include <cstddef>
#include <string_view>

#include "donner/css/parser/details/Tokenizer.h"

namespace donner::benchmarks {

/// Tiny inline style with one declaration.
inline constexpr std::string_view kInlineStyleShort = "fill:red";

/// Moderate inline style with several declarations.
inline constexpr std::string_view kInlineStyleMedium =
    "fill:#ff8040;stroke:rgb(0,128,255);stroke-width:2;stroke-linecap:round;"
    "stroke-linejoin:bevel;opacity:0.8;stroke-dasharray:4 2 1";

/// Short stylesheet with a handful of simple selectors and declarations.
inline constexpr std::string_view kStylesheetSmall =
    "svg { fill: red; stroke: black; }\n"
    "rect { fill: blue; }\n"
    ".outline { stroke-width: 2; stroke: currentColor; }\n";

/// Medium stylesheet with selectors and nested function values typical of an SVG style block.
inline constexpr std::string_view kStylesheetMedium = R"CSS(
svg { font-family: sans-serif; font-size: 14px; }
g.layer { opacity: 0.9; }
g.layer > rect { fill: #eeeeee; stroke: #333; }
rect.highlight { fill: url(#grad1); stroke-width: 2; }
rect.highlight:hover { fill: url(#grad2); }
circle[data-kind="pin"] { fill: hsl(200, 80%, 50%); }
circle[data-kind="pin"]:nth-child(2n+1) { fill: hsl(20, 80%, 50%); }
path.outline { fill: none; stroke: currentColor; stroke-width: 1.5; }
text { font-weight: 600; fill: #111; }
text.muted { fill: #888; font-style: italic; }
.marker { stroke-linecap: round; stroke-linejoin: round; }
g#legend > rect { fill: white; stroke: #ccc; }
g#legend > text { font-size: 12px; }
use[href="#icon-warn"] { fill: orange; }
.hidden { display: none !important; }
)CSS";

/// Selector with a class, attribute, pseudo-class, and combinator.
inline constexpr std::string_view kSelectorComplex =
    "div.container > .row[data-role=\"primary\"]:nth-child(2n+1):hover";

/// Consume every token in \p input and return the number consumed.
inline std::size_t ConsumeCssTokens(std::string_view input) {
  css::parser::details::Tokenizer tokenizer(input);
  std::size_t tokenCount = 0;
  while (!tokenizer.isEOF()) {
    auto token = tokenizer.next();
    ++tokenCount;
  }
  return tokenCount;
}

}  // namespace donner::benchmarks
