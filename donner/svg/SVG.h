#pragma once
/// @file

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/xml/XMLParser.h"

/**
 * Top-level Donner namespace, which is split into different sub-namespaces such as \ref donner::svg
 * and \ref donner::css.
 */
namespace donner {

/**
 * Donner SVG library, which can load, manipulate and render SVG files.
 *
 * Loading SVG files can be done using \ref XMLParser:
 * ```
 * std::vector<char> fileData = ...;
 *
 * std::vector<ParseError> warnings;
 * auto maybeResult = XMLParser::ParseSVG(fileData, &warnings);
 *
 * if (maybeResult.hasError()) {
 *   const auto& e = maybeResult.error();
 *   std::cerr << "Parse Error " << e.line << ":" << e.offset << ": " << e.reason << std::endl;
 *   exit(1);
 * }
 *
 * std::cout << "Parsed successfully." << std::endl;
 *
 * if (!warnings.empty()) {
 *   std::cout << "Warnings:" << std::endl;
 *   for (auto& w : warnings) {
 *     std::cout << "  " << w.line << ":" << w.offset << ": " << w.reason << std::endl;
 *  }
 * }
 * ```
 *
 * The resulting \ref SVGDocument can be used to traverse the SVG file:
 * ```
 * SVGDocument document = std::move(maybeResult.result());
 *
 * if (auto myPath = document.svgElement().querySelector("#myPath")) {
 *   std::cout << "Found #myPath" << std::endl;
 *   myPath->setStyle("fill: red");
 *   myPath->setStyle("stroke: white");
 * }
 * ```
 *
 * The SVG file can be rendered using \ref RendererSkia:
 * ```
 * #include "donner/svg/renderer/RendererSkia.h"
 *
 * RendererSkia renderer;
 * renderer.draw(document);
 *
 * if (renderer.save("output.png")) {
 *   std::cout << "Saved to output.png" << std::endl;
 * } else {
 *   std::cerr << "Failed to save to file" << std::endl;
 * }
 * ```
 */
namespace svg {}  // namespace svg

}  // namespace donner
