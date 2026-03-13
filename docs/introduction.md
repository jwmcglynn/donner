# Introduction {#Introduction}

\tableofcontents

Donner is an embeddable browser-grade SVG2 engine in C++20, providing full access to the SVG DOM with complete rendering support including text, filters, and animation.

\htmlonly <style>img[src="donner_splash.svg"]{max-width:800px;}</style> \endhtmlonly
![Donner splash image](donner_splash.svg)

Donner includes:

- SVG2 core functionality, such as shapes, fills, strokes, and gradients.
- All 17 SVG filter primitives (feGaussianBlur, feColorMatrix, feComposite, etc.).
- Text rendering with `<text>` and `<tspan>`, including WOFF2 web fonts and optional HarfBuzz shaping.
- SVG animation: `<animate>`, `<animateTransform>`, `<animateMotion>`, `<set>` with full timing model.
- CSS3 parsing and cascading support, with a hand-rolled library.
- A game-engine-inspired [EnTT](https://github.com/skypjack/entt) ECS-backed document tree.
- A SVG DOM-style API to traverse, inspect, and modify documents in memory.
- A two-phase renderer, which builds and caches a rendering tree for efficient frame-based rendering.
- Two renderer backends: **Skia** (Chromium's renderer) and **tiny-skia** (lightweight software renderer).

Donner focuses on security and performance, which is validated with code coverage and fuzz testing.

## Try it out: Render an SVG to PNG

```sh
bazel run --run_under="cd $PWD &&" //examples:svg_to_png -- donner_splash.svg
```

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/jwmcglynn/donner)

How it works: \ref svg_to_png.cc

## API Demo

\snippet svg_tree_interaction.cc homepage_snippet

Detailed docs: \ref svg_tree_interaction.cc

## Documentation

- \subpage GettingStarted
- \ref SystemArchitecture
- \ref Devtools
- \ref DonnerAPI
- [Examples](Examples.html)

## Project Goals

- Have minimal dependencies, so it can be integrated into existing applications, assuming a modern compiler.
- Expose the SVG DOM, so that applications can manipulate SVGs dynamically.
- Implement the [SVG 2 Specification](https://www.w3.org/TR/SVG2/).

## Status

- [Project status](https://github.com/jwmcglynn/donner/issues/149) (github)
- [Build report](build_report.md)

## Building

Donner is built using [Bazel](https://bazel.build/), and builds are tested on Linux and macOS.

See more details in the \ref BuildingDonner instructions.

<div class="section_buttons">

| Previous |                                   Next |
| :------- | -------------------------------------: |
|          | [Getting started](GettingStarted.html) |

</div>
