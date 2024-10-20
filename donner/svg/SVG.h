#pragma once
/// @file

#include "donner/svg/SVGDocument.h"    // IWYU pragma: export
#include "donner/svg/SVGElement.h"     // IWYU pragma: export
#include "donner/svg/xml/SVGParser.h"  // IWYU pragma: export

/**
 * Top-level Donner namespace, which is split into different sub-namespaces such as \ref donner::svg
 * and \ref donner::css.
 */
namespace donner {

/**
 * Donner SVG library, which can load, manipulate and render SVG files.
 *
 * Loading SVG files can be done using \ref donner::svg::parser::SVGParser
 * ```
 * SVGParser::InputBuffer svgSource("<svg>...</svg>");
 *
 * std::vector<ParseError> warnings;
 * auto maybeResult = SVGParser::ParseSVG(svgSource, &warnings);
 *
 * if (maybeResult.hasError()) {
 *   const auto& e = maybeResult.error();
 *   std::cerr << "Parse Error: " << e << "\n";
 *   exit(1);
 * }
 *
 * std::cout << "Parsed successfully.\n";
 *
 * if (!warnings.empty()) {
 *   std::cout << "Warnings:\n";
 *   for (auto& w : warnings) {
 *     std::cout << "  " << w << "\n";
 *  }
 * }
 * ```
 *
 * The resulting \ref SVGDocument can be used to traverse the SVG file:
 * ```
 * SVGDocument document = std::move(maybeResult.result());
 *
 * if (auto myPath = document.querySelector("#myPath")) {
 *   std::cout << "Found #myPath" << std::endl;
 *   myPath->setStyle("fill: red");
 *   myPath->setStyle("stroke: white");
 * }
 * ```
 *
 * The SVG file can be rendered using \ref donner::svg::RendererSkia
 * ```
 * #include "donner/svg/renderer/RendererSkia.h"
 *
 * RendererSkia renderer;
 * renderer.draw(document);
 *
 * if (renderer.save("output.png")) {
 *   std::cout << "Saved to output.png\n";
 * } else {
 *   std::cerr << "Failed to save to file\n";
 * }
 * ```
 */
namespace svg {}  // namespace svg

}  // namespace donner
