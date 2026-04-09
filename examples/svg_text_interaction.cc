/**
 * @example svg_text_interaction.cc SVG Text DOM Manipulation
 *
 * Demonstrates how to create, inspect, and modify `<text>` content through the Donner DOM.
 * Builds a new SVG document from scratch containing a `<text>` element with multiple
 * `<tspan>` runs, then queries, re-styles, and appends more content, and finally renders the
 * result to a PNG.
 *
 * ```sh
 * bazel run //examples:svg_text_interaction -- output.png
 * ```
 */

#include <iostream>

#include "donner/base/Length.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/svg/SVG.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/SVGTSpanElement.h"
#include "donner/svg/renderer/Renderer.h"

using donner::Lengthd;
using donner::svg::SVGDocument;
using donner::svg::SVGElement;
using donner::svg::SVGTextElement;
using donner::svg::SVGTSpanElement;

int main(int argc, char* argv[]) {
  // Parse an initial document with one <text> element and two <tspan> runs. In a real
  // application you would typically load this from a file, but the embedded snippet here
  // keeps the example self-contained.
  //! [text_svg_source]
  const std::string_view kSvgSource(R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="400" height="120" viewBox="0 0 400 120">
      <text x="20" y="70" font-family="sans-serif" font-size="32" fill="#1f5a8a">
        Hello, <tspan fill="#c33" font-weight="bold">Donner</tspan>!
      </text>
    </svg>
  )");
  //! [text_svg_source]

  donner::ParseWarningSink warnings;
  auto maybeDocument =
      donner::svg::parser::SVGParser::ParseSVG(kSvgSource, warnings);
  if (maybeDocument.hasError()) {
    std::cerr << "Parse error: " << maybeDocument.error() << "\n";
    return 1;
  }
  SVGDocument document = std::move(maybeDocument.result());

  //! [text_query]
  // Find the <text> element via a CSS selector and read its current content.
  std::optional<SVGElement> maybeText = document.querySelector("text");
  UTILS_RELEASE_ASSERT_MSG(maybeText, "Expected a <text> element in the document");
  SVGTextElement text = maybeText->cast<SVGTextElement>();

  std::cout << "Current text content: \"" << text.textContent() << "\"\n";
  //! [text_query]

  //! [text_restyle]
  // Restyle the <text> element. Styles compose with any existing values instead of replacing
  // them, so this leaves the inherited fill color on the nested <tspan> runs alone.
  text.setStyle("font-size: 40px");
  text.setStyle("font-weight: 500");
  text.setX(Lengthd(20));
  text.setY(Lengthd(80));
  //! [text_restyle]

  //! [text_append_tspan]
  // Append a new <tspan> run to the end of the text. New elements are attached via the same
  // DOM API used for shapes: create them, configure their properties, then insert them into
  // the tree.
  SVGTSpanElement newRun = SVGTSpanElement::Create(document);
  newRun.appendText(" v0.5");
  newRun.setStyle("fill: #2a9");
  newRun.setStyle("font-style: italic");
  text.appendChild(newRun);
  //! [text_append_tspan]

  std::cout << "After append, text content: \"" << text.textContent() << "\"\n";

  //! [text_render]
  // Render the modified document to PNG.
  const std::string output = argc >= 2 ? argv[1] : "text_interaction.png";
  donner::svg::Renderer renderer;
  renderer.draw(document);
  if (!renderer.save(output.c_str())) {
    std::cerr << "Failed to save " << output << "\n";
    return 1;
  }
  std::cout << "Rendered to " << output << " (" << renderer.width() << "x" << renderer.height()
            << ")\n";
  //! [text_render]

  return 0;
}
