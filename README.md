# Donner, a modern SVG rendering library in C++

[![Build Status](https://github.com/jwmcglynn/donner/actions/workflows/main.yml/badge.svg)](https://github.com/jwmcglynn/donner/actions/workflows/main.yml) [![License: ISC](https://img.shields.io/badge/License-ISC-blue.svg)](https://opensource.org/licenses/ISC) [![Code coverage %](https://codecov.io/gh/jwmcglynn/donner/branch/main/graph/badge.svg?token=Z3YJZNKGU0)](https://codecov.io/gh/jwmcglynn/donner) ![Product lines of code](https://gist.githubusercontent.com/jwmcglynn/91f7f490a72af9c06506c8176729d218/raw/loc.svg) ![Test lines of code](https://gist.githubusercontent.com/jwmcglynn/91f7f490a72af9c06506c8176729d218/raw/loc-tests.svg)
![Comments %](https://gist.githubusercontent.com/jwmcglynn/91f7f490a72af9c06506c8176729d218/raw/comments.svg)

Donner is an under-development modern C++20 SVG rendering library which provides full access to the SVG DOM, enabling browser-level functionality without the browser.

![Donner splash image](donner_splash.svg)

Currently, Donner includes:

- SVG2 core functionality, such as shapes, fills, strokes, and gradients
- CSS3 parsing and cascading support, with a hand-rolled library
- A game-engine-inspired [EnTT](https://github.com/skypjack/entt) ECS-backed document tree
- A SVG DOM-style API to traverse, inspect, and modify documents in memory
- A two-phase renderer, which builds and caches a rendering tree for efficient frame-based rendering

Donner renders with Skia, which provides the same high-quality rendering used by Chromium.

Donner focuses on security and performance, which is validated with code coverage and fuzz testing.

## Supported Elements

[`<circle>`](https://jwmcglynn.github.io/donner/group__elements__basic__shapes.html#xml_circle) [`<clipPath>`](https://jwmcglynn.github.io/donner/group__elements__structural.html#xml_clipPath) [`<defs>`](https://jwmcglynn.github.io/donner/group__elements__structural.html#xml_defs) [`<ellipse>`](https://jwmcglynn.github.io/donner/group__elements__basic__shapes.html#xml_ellipse) [`<g>`](https://jwmcglynn.github.io/donner/group__elements__structural.html#xml_g) [`<image>`](https://jwmcglynn.github.io/donner/group__xml__image.html) [`<line>`](https://jwmcglynn.github.io/donner/group__elements__basic__shapes.html#xml_line) [`<linearGradient>`](https://jwmcglynn.github.io/donner/group__elements__paint__servers.html#xml_linearGradient) [`<marker>`](https://jwmcglynn.github.io/donner/group__xml__marker.html) [`<mask>`](https://jwmcglynn.github.io/donner/group__xml__mask.html) [`<path>`](https://jwmcglynn.github.io/donner/group__elements__basic__shapes.html#xml_path) [`<pattern>`](https://jwmcglynn.github.io/donner/group__elements__paint__servers.html#xml_pattern) [`<polygon>`](https://jwmcglynn.github.io/donner/group__elements__basic__shapes.html#xml_polygon) [`<polyline>`](https://jwmcglynn.github.io/donner/group__elements__basic__shapes.html#xml_polyline) [`<radialGradient>`](https://jwmcglynn.github.io/donner/group__elements__paint__servers.html#xml_radialGradient) [`<rect>`](https://jwmcglynn.github.io/donner/group__elements__basic__shapes.html#xml_rect) [`<stop>`](https://jwmcglynn.github.io/donner/group__elements__paint__servers.html#xml_stop) [`<style>`](https://jwmcglynn.github.io/donner/group__xml__style.html) [`<svg>`](https://jwmcglynn.github.io/donner/group__elements__structural.html#xml_svg) [`<use>`](https://jwmcglynn.github.io/donner/group__elements__structural.html#xml_use)

**Not yet supported:** `<a>` `<filter>` `<switch>` `<symbol>` `<text>` `<textPath>` `<tspan>`

## Try it out: Render an SVG to PNG

```sh
bazel run --run_under="cd $PWD &&" //examples:svg_to_png -- donner_splash.svg
```

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/jwmcglynn/donner)

How it works: [svg_to_png.cc](https://jwmcglynn.github.io/donner/svg_to_png_8cc-example.html)

## API Demo

```cpp
// This is the base SVG we are loading, a simple path containing a line
donner::svg::parser::XMLParser::InputBuffer svgContents(R"(
  <svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 10 10">
    <path d="M 1 1 L 4 5" stroke="blue" />
  </svg>
)");

// Call ParseSVG to load the SVG file
donner::base::parser::ParseResult<donner::svg::SVGDocument> maybeResult =
    donner::svg::parser::XMLParser::ParseSVG(svgContents);

if (maybeResult.hasError()) {
  std::cerr << "Parse Error " << maybeResult.error() << "\n";  // Includes line:column and reason
  std::abort();
  // - or - handle the error per your project's conventions
}

donner::svg::SVGDocument document = std::move(maybeResult.result());

// querySelector supports standard CSS selectors, anything that's valid when defining a CSS rule
// works here too, for example querySelector("svg > path[fill='blue']") is also valid and will
// match the same element.
std::optional<donner::svg::SVGElement> maybePath = document.querySelector("path");
UTILS_RELEASE_ASSERT_MSG(maybePath, "Failed to find path element");

// The result of querySelector is a generic SVGElement, but we know it's a path, so we can cast
// it. If the cast fails, an assertion will be triggered.
donner::svg::SVGPathElement path = maybePath->cast<donner::svg::SVGPathElement>();

if (std::optional<donner::svg::PathSpline> spline = path.computedSpline()) {
  std::cout << "Path: " << *spline << "\n";
  std::cout << "Length: " << spline->pathLength() << " userspace units\n";
} else {
  std::cout << "Path is empty\n";
}
```

Detailed docs: [svg_tree_interaction.cc](https://jwmcglynn.github.io/donner/svg_tree_interaction_8cc-example.html)

## API Demo 2: Rendering a SVG to PNG

```cpp
using namespace donner::base;
using namespace donner::base::parser;
using namespace donner::svg;
using namespace donner::svg::parser;

std::ifstream file("test.svg");
if (!file) {
  std::cerr << "Could not open file\n";
  std::abort();
}

XMLParser::InputBuffer fileData;
fileData.loadFromStream(file);

ParseResult<SVGDocument> maybeDocument = XMLParser::ParseSVG(fileData);
if (maybeDocument.hasError()) {
  std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
  std::abort();
}

RendererSkia renderer;
renderer.draw(maybeDocument.result());

const bool success = renderer.save("output.png");
```

Detailed docs: [svg_to_png.cc](https://jwmcglynn.github.io/donner/svg_to_png_8cc-example.html)

## API Demo 3: Interactive SVG Viewer using ImGui

```sh
bazel run --run_under="cd $PWD &&" //examples:svg_viewer -- <filename>
```

This example demonstrates how to create an interactive SVG viewer using ImGui. The viewer allows you to load and display SVG files, and interact with SVG elements using ImGui.

Detailed docs: [svg_viewer.cc](https://jwmcglynn.github.io/donner/svg_viewer_8cc-example.html)

## Documentation

- [Getting started](https://jwmcglynn.github.io/donner/GettingStarted.html)
- [API Documentation](https://jwmcglynn.github.io/donner/DonnerAPI.html)
- [System architecture](https://jwmcglynn.github.io/donner/SystemArchitecture.html)
- [Building Donner](https://jwmcglynn.github.io/donner/BuildingDonner.html)
- [Examples](https://jwmcglynn.github.io/donner/examples.html)

## Status

- [Project status](https://github.com/jwmcglynn/donner/issues/149)
- [Build report](docs/build_report.md)

## Other Libraries

- C++ | **[LunaSVG](https://github.com/sammycage/lunasvg)**: A lightweight library with an embedded renderer, suitable for embedded applications
- Rust | **[librsvg](https://gitlab.gnome.org/GNOME/librsvg)**: Provides a simple way to render SVGs one-shot, does not provide a DOM or animation
- Rust | **[resvg](https://github.com/RazrFalcon/resvg)**: Library that focuses on correctness, safety, and portability for static SVGs
