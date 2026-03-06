# Introduction {#Introduction}

\tableofcontents

Donner is an under-development modern C++20 SVG rendering library which provides full access to the SVG DOM, enabling browser-level functionality without the browser.

\htmlonly <style>img[src="donner_splash.svg"]{max-width:800px;}</style> \endhtmlonly
![Donner splash image](donner_splash.svg)

Currently, Donner includes:

- SVG2 core functionality, such as shapes, fills, strokes, and gradients.
- CSS3 parsing and cascading support, with a hand-rolled library.
- A game-engine-inspired [EnTT](https://github.com/skypjack/entt) ECS-backed document tree.
- A SVG DOM-style API to traverse, inspect, and modify documents in memory.
- A two-phase renderer, which builds and caches a rendering tree for efficient frame-based rendering.

Donner supports two rendering backends, selected at build time:
- **TinySkia** (default): A lightweight software rasterizer for fast builds with no system dependencies.
- **Skia**: Full-featured rendering via Google Skia, used as a reference renderer.

See the \ref BuildingDonner instructions for backend selection.

Donner focuses on security and performance, which is validated with code coverage and fuzz testing.

## Try it out: Render an SVG to PNG

```sh
bazel run --run_under="cd $PWD &&" //examples:svg_to_png -- donner_splash.svg
```

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
