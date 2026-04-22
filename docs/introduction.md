# Introduction {#Introduction}

\tableofcontents

Donner is an embeddable browser-grade SVG2 engine in C++20, providing full access to the SVG DOM with complete rendering support including text and filters.

\htmlonly <style>img[src="donner_splash.svg"]{max-width:800px;}</style> \endhtmlonly
![Donner splash image](donner_splash.svg)

Donner supports:

- SVG2 core functionality, such as shapes, fills, strokes, and gradients.
- Text rendering with `<text>`, `<tspan>`, and `<textPath>`, including WOFF2 web fonts and optional HarfBuzz shaping.
- All 17 SVG filter primitives (feGaussianBlur, feColorMatrix, feComposite, etc.).
- CSS3 parsing and cascading support, with a hand-rolled library.
- Detailed validation and diagnostics, errors point to the exact location.
- A game-engine-inspired [EnTT](https://github.com/skypjack/entt) ECS-backed document tree, optimized for performance.
- A SVG DOM-style API to traverse, inspect, and modify documents in memory.
- A two-phase renderer, which builds and caches a rendering tree for efficient frame-based rendering.
- Two renderer backends: **tiny-skia** (default, a lightweight software renderer) and **Skia** (Chromium's renderer).

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

- \subpage GettingStarted
- \ref DonnerAPI
- [Examples](examples.html)
- \subpage DeveloperDocs

## Project Goals

- Have minimal dependencies, so it can be integrated into existing applications, assuming a modern compiler.
- Expose the SVG DOM, so that applications can manipulate SVGs dynamically.
- Implement the [SVG 2 Specification](https://www.w3.org/TR/SVG2/).

## Status

- [Project Status](https://github.com/jwmcglynn/donner/issues/149) (github)
- [Build Report](build_report.md)

## Building

Donner is built using [Bazel](https://bazel.build/), and builds are tested on Linux and macOS.

See more details in the \ref BuildingDonner instructions.

<div class="section_buttons">

| Previous |                                   Next |
| :------- | -------------------------------------: |
|          | [Getting Started](GettingStarted.html) |

</div>
