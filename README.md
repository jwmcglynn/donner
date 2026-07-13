# Donner SVG Editor & Engine

[![Build Status](https://github.com/jwmcglynn/donner/actions/workflows/main.yml/badge.svg)](https://github.com/jwmcglynn/donner/actions/workflows/main.yml) [![License: ISC](https://img.shields.io/badge/License-ISC-blue.svg)](https://opensource.org/licenses/ISC) [![Code coverage %](https://codecov.io/gh/jwmcglynn/donner/branch/main/graph/badge.svg?token=Z3YJZNKGU0)](https://codecov.io/gh/jwmcglynn/donner) ![Product lines of code](https://gist.githubusercontent.com/jwmcglynn/91f7f490a72af9c06506c8176729d218/raw/loc.svg) ![Test lines of code](https://gist.githubusercontent.com/jwmcglynn/91f7f490a72af9c06506c8176729d218/raw/loc-tests.svg)
![Comments %](https://gist.githubusercontent.com/jwmcglynn/91f7f490a72af9c06506c8176729d218/raw/comments.svg)

A native SVG editor, built on its own SVG2 + CSS3 engine, written from scratch in C++20.

Donner is an SVG-native editor: a fast, native tool for creating and editing SVG, backed by an engine
built for browser-grade correctness, security, and performance. The same engine that powers
the editor embeds cleanly into your application, from a full GPU-rendered canvas down to a
size-optimized software renderer.

![Donner splash image](donner_splash.svg)

[Try it out online!](https://jwmcglynn.github.io/donner-editor/)

## Why Donner

- An SVG-native editor. Selection and transform tools with oriented bounding boxes, a pen
  tool, rich text editing with real font support, and layers. Every icon and cursor is an
  SVG rendered by Donner itself; the document you edit is the SVG, always.
- High SVG spec conformance. Conformance is tracked continuously against the resvg test suite with visual regression in CI.
- Fully featured. SVG2 rendering with CSS3 styling, text with a full font stack (FreeType,
  HarfBuzz, WOFF2) or a compact built-in stack, filters, and the first slice of SMIL
  animation with time-sampled rendering.
- High performance. Geode, a GPU renderer built on WebGPU, drives the editor canvas; a
  tiny_skia-based CPU backend serves the size-optimized embeddable build.
- Secure. Built for untrusted content: 24 fuzzers run continuously, and the
  engine's design lineage includes shipping untrusted SVG and glTF rendering inside
  privileged apps.
- Embeddable. A C++20 Bazel module with an exception-free, RTTI-free API surface; the tiny
  variant is tuned for binary size.

## The editor

The editor ships with the engine and is under active development. Open a file, edit visually or in the built-in XML view with two-way sync, and export clean SVG.

## Supported SVG elements and features

Donner targets the SVG 2 static rendering subset. The tables below are an honest snapshot of what
the engine actually parses and renders today, derived from the code and from the conformance run in
`donner/svg/renderer/tests/resvg_test_suite.cc`, which pixel-compares Donner against the upstream
[resvg test suite](https://github.com/RazrFalcon/resvg-test-suite) goldens on every push. That test
and the SVG 2 conformance program in [design 0057](docs/design_docs/0057-donner_svg2_test_suite.md)
are the measurement backbone; this section is the interim human-readable summary until the
conformance suite publishes a generated gap report.

Legend: **Yes** = parsed and rendered; **Partial** = supported with the noted gaps;
**Parsed only** = retained on the DOM but not drawn (by design); **Experimental** = implemented but
off by default; **No** = not recognized (parses to an unknown element).

### Elements

| Category | Elements | Support |
| :------- | :------- | :------ |
| Structural | [`<svg>`](https://jwmcglynn.github.io/donner/xml_svg.html) [`<g>`](https://jwmcglynn.github.io/donner/xml_g.html) [`<defs>`](https://jwmcglynn.github.io/donner/xml_defs.html) [`<symbol>`](https://jwmcglynn.github.io/donner/xml_symbol.html) [`<use>`](https://jwmcglynn.github.io/donner/xml_use.html) [`<style>`](https://jwmcglynn.github.io/donner/xml_style.html) [`<switch>`](https://jwmcglynn.github.io/donner/xml_switch.html) | Yes |
| Hyperlink | [`<a>`](https://jwmcglynn.github.io/donner/xml_a.html) | Yes. Renders as a transparent group (its children draw in place); the link target (`href` / `xlink:href`) is retained on the DOM. |
| Shapes | [`<circle>`](https://jwmcglynn.github.io/donner/xml_circle.html) [`<ellipse>`](https://jwmcglynn.github.io/donner/xml_ellipse.html) [`<line>`](https://jwmcglynn.github.io/donner/xml_line.html) [`<path>`](https://jwmcglynn.github.io/donner/xml_path.html) [`<polygon>`](https://jwmcglynn.github.io/donner/xml_polygon.html) [`<polyline>`](https://jwmcglynn.github.io/donner/xml_polyline.html) [`<rect>`](https://jwmcglynn.github.io/donner/xml_rect.html) | Yes |
| Raster image | [`<image>`](https://jwmcglynn.github.io/donner/xml_image.html) | Partial. Embedded (data URI) images render; external-URL and cross-file references are not loaded. |
| Text | [`<text>`](https://jwmcglynn.github.io/donner/xml_text.html) [`<tspan>`](https://jwmcglynn.github.io/donner/xml_tspan.html) [`<textPath>`](https://jwmcglynn.github.io/donner/xml_textPath.html) | Partial. See Text features below. Text requires a text-enabled build; the size-optimized build can omit it. |
| Paint servers and markers | [`<linearGradient>`](https://jwmcglynn.github.io/donner/xml_linearGradient.html) [`<radialGradient>`](https://jwmcglynn.github.io/donner/xml_radialGradient.html) [`<stop>`](https://jwmcglynn.github.io/donner/xml_stop.html) [`<pattern>`](https://jwmcglynn.github.io/donner/xml_pattern.html) [`<marker>`](https://jwmcglynn.github.io/donner/xml_marker.html) | Yes. Linear and radial gradients (all spread methods, radial focal point) and patterns are supported; conic/sweep gradients are not. |
| Masking and clipping | [`<mask>`](https://jwmcglynn.github.io/donner/xml_mask.html) [`<clipPath>`](https://jwmcglynn.github.io/donner/xml_clipPath.html) | Partial. Core masking and clipping work; clip on/with `<text>`, some nested clip-path intersections, and a few mask-unit edge cases are not yet handled. |
| Filters | [`<filter>`](https://jwmcglynn.github.io/donner/xml_filter.html) and the full `<fe*>` primitive suite | Partial. All 17 filter primitives have DOM wrappers and renderer support; the CSS `filter:` function-list category, `enable-background` / `BackgroundImage` inputs, and some feImage subregion cases are not covered. See the [filter element reference](https://jwmcglynn.github.io/donner/elements_filters.html). |
| Descriptive | [`<title>`](https://jwmcglynn.github.io/donner/xml_title.html) [`<desc>`](https://jwmcglynn.github.io/donner/xml_desc.html) [`<metadata>`](https://jwmcglynn.github.io/donner/xml_metadata.html) | Parsed only (retained, never drawn, per spec). |
| Animation (SMIL) | `<animate>` `<animateTransform>` `<set>` | Experimental, off by default. An animation system exists and renders time-sampled frames, but these elements parse to their animation types only when experimental parsing is enabled; otherwise they are treated as unknown. |

Elements outside this list (for example `<foreignObject>`, `<tref>`, SVG 1.1 `<font>` / `<glyph>`,
`<cursor>`, `<view>`, `<script>`) are not recognized and parse to an unknown element. See
[unsupported SVG 1.x features](docs/unsupported_svg1_features.md) for the SVG 1.1 details.

### Presentation attributes and CSS properties

| Support | Properties |
| :------ | :--------- |
| Honored | `fill` / `fill-rule` / `fill-opacity`, `stroke` and all `stroke-*`, `opacity`, `color`, `display`, `visibility`, `overflow`, `transform` / `transform-origin`, `clip-path` / `clip-rule`, `mask`, `filter`, `color-interpolation-filters`, `marker-start` / `-mid` / `-end`, `mix-blend-mode` (all 16 modes), `isolation`, `image-rendering` (sampling), `vector-effect` (only `non-scaling-stroke`), and the text properties (`font-*`, `text-anchor`, `text-decoration`, baseline family, `letter-spacing`, `word-spacing`, `writing-mode`). |
| Partial | `paint-order` (honored by the CPU backend; not yet by the GPU backend). |
| Parsed but not painted | `pointer-events` and `cursor` (used for hit-testing / interaction, not drawing); the rendering hints `color-rendering`, `shape-rendering`, `text-rendering`, `color-interpolation`. |
| Not implemented | `direction` / `unicode-bidi` (bidirectional text), `image-rendering: pixelated` / `crisp-edges`, `glyph-orientation-*`, `font-size-adjust`, CSS2 `clip: rect(...)`. |

### Text features

Text layout supports fills and strokes (including gradient and pattern paints), per-glyph
positioning, `text-anchor`, `textPath` along a path, and a full font stack (FreeType, HarfBuzz,
WOFF2) or a compact built-in stack. Known gaps, tracked against the resvg suite: bidirectional
text and `direction` / `unicode-bidi`, `textLength` / `lengthAdjust`, several SVG 2 `<textPath>`
features (`side`, `method=stretch`, `spacing=auto`, the `path` attribute), full SVG 2
`text-decoration` (independent line, style, and color), `<tref>`, and some vertical
`writing-mode: tb` cases.

### Renderers

Donner ships two backends behind one renderer interface. The tiny_skia CPU backend is the default
and the most complete; the Geode WebGPU backend drives the editor canvas and is at broad parity,
with a few narrower gaps (`paint-order`, some `0 N` dash caps, nested path-clip intersection) where
the CPU backend currently passes conformance cases the GPU backend does not. Both share the same
DOM, layout, paint resolution, markers, and filter graph.

## CLI Tool: donner-svg

Donner also ships an end-user CLI for rendering and previewing SVG files.

```sh
# Render to PNG
bazel run //donner/svg/tool:donner-svg -- donner_splash.svg --output output.png

# Show a terminal preview
bazel run //donner/svg/tool:donner-svg -- donner_splash.svg --preview

# Interactive terminal mode with mouse selection
bazel run //donner/svg/tool:donner-svg -- donner_splash.svg --interactive
```

Tool docs: [donner-svg CLI tool](https://jwmcglynn.github.io/donner/DonnerSvgTool.html)

## Simplified Example: Saving an SVG to PNG

```sh
bazel run //examples:svg_to_png -- donner_splash.svg
```

How it works: [svg_to_png.cc](https://jwmcglynn.github.io/donner/svg_to_png_8cc-example.html)

## API Demo

```cpp
// This is the base SVG we are loading, a simple path containing a line
const std::string_view svgContents(R"(
  <svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 10 10">
    <path d="M 1 1 L 4 5" stroke="blue" />
  </svg>
)");

// Call ParseSVG to load the SVG file
donner::ParseWarningSink disabled = donner::ParseWarningSink::Disabled();
donner::ParseResult<donner::svg::SVGDocument> maybeResult =
    donner::svg::parser::SVGParser::ParseSVG(svgContents, disabled);

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

if (std::optional<donner::Path> computedPath = path.computedPath()) {
  std::cout << "Path: " << *computedPath << "\n";
  std::cout << "Length: " << computedPath->pathLength() << " userspace units\n";
} else {
  std::cout << "Path is empty\n";
}
```

Detailed docs: [svg_tree_interaction.cc](https://jwmcglynn.github.io/donner/svg_tree_interaction_8cc-example.html)

## API Demo 2: Rendering a SVG to PNG

```cpp
using namespace donner;
using namespace donner::svg;
using namespace donner::svg::parser;

std::ifstream file("test.svg");
if (!file) {
  std::cerr << "Could not open file\n";
  std::abort();
}

std::string fileData;
file.seekg(0, std::ios::end);
const std::streamsize fileLength = file.tellg();
file.seekg(0);

fileData.resize(fileLength);  
file.read(fileData.data(), fileLength);

ParseWarningSink warnings;
ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(fileData, warnings);
if (maybeDocument.hasError()) {
  std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
  std::abort();
}

Renderer renderer;
renderer.draw(maybeDocument.result());

const bool success = renderer.save("output.png");
```

Detailed docs: [svg_to_png.cc](https://jwmcglynn.github.io/donner/svg_to_png_8cc-example.html)

## Documentation

- [Getting Started](https://jwmcglynn.github.io/donner/GettingStarted.html)
- [API Documentation](https://jwmcglynn.github.io/donner/DonnerAPI.html)
- [System Architecture](https://jwmcglynn.github.io/donner/SystemArchitecture.html)
- [Building Donner](https://jwmcglynn.github.io/donner/BuildingDonner.html)
- [donner-svg CLI tool](https://jwmcglynn.github.io/donner/DonnerSvgTool.html)
- [Examples](https://jwmcglynn.github.io/donner/examples.html)

## Status

- [Project Status](https://github.com/jwmcglynn/donner/issues/149)
- [Build Report](docs/build_report.md)

## CMake Support

CMake support is available for integrating Donner into CMake-based projects. The CMake build fetches dependencies and builds the library. Both the tiny_skia (CPU) and Geode (WebGPU) backends are selectable via `DONNER_RENDERER_BACKEND` (default `tiny_skia`).

See the [CMake Documentation](https://jwmcglynn.github.io/donner/BuildingDonner.html#cmake-build) for more details.

## Other Libraries

- C++ | **[LunaSVG](https://github.com/sammycage/lunasvg)**: A lightweight library with an embedded renderer, suitable for embedded applications
- Rust | **[librsvg](https://gitlab.gnome.org/GNOME/librsvg)**: Provides a simple way to render SVGs one-shot, does not provide a DOM or animation
- Rust | **[resvg](https://github.com/RazrFalcon/resvg)**: Library that focuses on correctness, safety, and portability for static SVGs
