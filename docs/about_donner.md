# About Donner

Donner is an under-development modern C++20 SVG rendering library which provides full access to the SVG DOM, enabling browser-level functionality without the browser.

\htmlonly <style>img[src="donner_splash.svg"]{width:50%;}</style> \endhtmlonly
![Donner splash image](donner_splash.svg)

Currently, Donner includes:

- SVG2 core functionality, such as shapes, fills, strokes, and gradients.
- CSS3 parsing and cascading support, with a hand-rolled library.
- An [EnTT](https://github.com/skypjack/entt) ECS-backed document tree.
- A SVG DOM-style API to traverse, inspect, and modify documents in memory.
- A two-phase renderer, which builds and caches a rendering tree for efficient frame-based rendering.

Donner currently renders with Skia as core functionality is being implemented. While Skia is powerful, it adds a lot of code size so alternative approaches may be considered in the future.

Donner focuses on security and performance, which is validated with code coverage and fuzz testing.

## Documentation

- [Getting started](getting_started.md)
- [API Documentation](namespaces.html)
- [System Architecture](architecture.md)
- [Devtools](devtools/index.md), such as binary size and code coverage

## Project Goals

- Have minimal dependencies, so it can be integrated into existing applications, assuming a modern compiler.
- Expose the SVG DOM, so that applications can manipulate SVGs dynamically.
- Implement the [SVG 2 Specification](https://www.w3.org/TR/SVG2/).

## Building

Donner is built using [Bazel](https://bazel.build/), and builds are tested on Linux and macOS.

To run the tests, run:

```sh
bazel test //...
```

To render an SVG as an image, try the renderer tool:

```sh
bazel run //src/svg/renderer:renderer_tool -- src/svg/renderer/testdata/Ghostscript_Tiger.svg
```

See more details in the [Building Donner](internal/building.md) instructions.
