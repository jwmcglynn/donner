# Introduction {#Introduction}

\tableofcontents

Donner SVG Editor & Engine is a native SVG editor and the embeddable C++20 engine underneath it. The engine renders
SVG2 with CSS3 styling through either Geode (a GPU renderer built on WebGPU) or a compact CPU
backend. Conformance is tracked against the resvg test suite, and all input is treated as
untrusted: the parser, style, and text subsystems are fuzzed continuously.

These pages document both the engine and the editor. Start with the core document model below,
or see \ref EditorDocs and \ref EditorArchitecture for the editor application.

\htmlonly <style>img[src="donner_splash.svg"]{max-width:800px;}</style> \endhtmlonly
![Donner splash image](donner_splash.svg)

Donner supports:

- SVG2 core functionality, such as shapes, fills, strokes, and gradients.
- Text rendering with `<text>`, `<tspan>`, and `<textPath>`, including WOFF2 web fonts and optional HarfBuzz shaping.
- All 17 SVG filter primitives (`feGaussianBlur`, `feColorMatrix`, `feComposite`, and the rest).
- CSS3 parsing and cascading, implemented in-house.
- Detailed validation and diagnostics; errors report the exact source location.
- A document tree optimized for inspection, mutation, and rendering.
- An SVG DOM-style API for traversing, inspecting, and modifying documents in memory.
- A two-phase renderer that builds and caches a rendering tree for efficient per-frame rendering.
- Two renderer backends: **tiny_skia** (a compact CPU software renderer) and **Geode** (a GPU renderer built on WebGPU).

Security and performance work is backed by code coverage and continuous fuzzing.

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

- Keep dependencies minimal so the engine can be integrated into existing applications with a modern compiler.
- Expose the SVG DOM so applications can manipulate documents dynamically.
- Implement the [SVG 2 Specification](https://www.w3.org/TR/SVG2/).

## Status

- [Project Status](https://github.com/jwmcglynn/donner/issues/149) (GitHub)
- \ref DonnerBuildReport

## Building

Donner builds with [Bazel](https://bazel.build/) and is tested on Linux and macOS. See
\ref BuildingDonner for details.

<div class="section_buttons">

| Previous |                                   Next |
| :------- | -------------------------------------: |
|          | [Getting Started](GettingStarted.html) |

</div>
