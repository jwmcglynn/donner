# Introduction {#Introduction}

\tableofcontents

Donner SVG Editor & Engine is an SVG-native editor and the C++20 engine underneath it,
designed to be embedded. The engine renders
SVG2 with CSS3 styling through either Geode (a GPU renderer built on WebGPU) or a compact
CPU backend, tracks conformance against the resvg test suite, and treats all input as
untrusted: fuzzing runs continuously across the parser, style, and text subsystems.

These pages document the engine API. Start with the core document model below; the editor
is documented separately in the repository.

\htmlonly <style>img[src="donner_splash.svg"]{max-width:800px;}</style> \endhtmlonly
![Donner splash image](donner_splash.svg)

Donner supports:

- SVG2 core functionality, such as shapes, fills, strokes, and gradients.
- Text rendering with `<text>`, `<tspan>`, and `<textPath>`, including WOFF2 web fonts and optional HarfBuzz shaping.
- All 17 SVG filter primitives (feGaussianBlur, feColorMatrix, feComposite, etc.).
- CSS3 parsing and cascading support, with a hand-rolled library.
- Detailed validation and diagnostics, errors point to the exact location.
- A performance-oriented document tree optimized for dynamic inspection, mutation, and rendering.
- A SVG DOM-style API to traverse, inspect, and modify documents in memory.
- A two-phase renderer, which builds and caches a rendering tree for efficient frame-based rendering.
- Two renderer backends: **tiny_skia** (a compact CPU software renderer) and **Geode** (a GPU renderer built on WebGPU).

Donner focuses on security and performance, which is validated with code coverage and fuzz testing.

## Try It Out: Render an SVG to PNG

```sh
bazel run //examples:svg_to_png -- donner_splash.svg
```

How it works: \ref svg_to_png.cc

## API Demo

\snippet svg_tree_interaction.cc homepage_snippet

Detailed docs: \ref svg_tree_interaction.cc

## Documentation

- \subpage Documentation
- \ref DonnerAPI
- [Examples](examples.html)

## Project Goals

- Have minimal dependencies, so it can be integrated into existing applications, assuming a modern compiler.
- Expose the SVG DOM, so that applications can manipulate SVGs dynamically.
- Implement the [SVG 2 Specification](https://www.w3.org/TR/SVG2/).

## Status

- [Project Status](https://github.com/jwmcglynn/donner/issues/149) (github)
- \ref DonnerBuildReport

## Building

Donner is built using [Bazel](https://bazel.build/), and builds are tested on Linux and macOS.

See more details in the \ref BuildingDonner instructions.

<div class="section_buttons">

| Previous |                                   Next |
| :------- | -------------------------------------: |
|          | [Getting Started](GettingStarted.html) |

</div>
