# About Donner {#mainpage}

Donner is a modern C++20 SVG rendering library that:
- Implements **SVG2**-natively
- Renders using the world-class **Skia** graphics library
- Allows modifying (and creating) SVG2 files entirely in C++
- Is designed for performance, using game-industry-vetted techniques such as an **EnTT** ECS-backed DOM tree

Donner also provides a production-grade **CSS3** library, which is usable independent of Donner SVG, that provides a hand-written parser, as well as Selectors (Level 4), to not only support parsing a CSS, but also to match it against a tree.

Donner is tested at every layer, and is secure from the start: utilizing fuzzers to ensure that it is resilient to invalid SVG and CSS.
- This is validated with full CI and coverage analysis

## Donner's Goals

- Have minimal dependencies, so it can be integrated into existing applications
- Expose the SVG DOM, so that applications can manipulate SVGs dynamically
- Implement the [SVG 2 Specification](https://www.w3.org/TR/SVG/)

## Donner's Non-Goals

- Provide a feature-complete implementation of the SVG spec. Donner aims to be compatible, but may implement a subset of the features for performance reasons
- Compile on a broad range of platforms/compilers. Donner is primarily intended as a playground for C++17/C++20, compiling with the latest clang/libc++. (This may be subject to change)

## Building

Donner is built using [Bazel](https://bazel.build/), and builds are tested on Linux and macOS.

To run the tests, run:
```
bazel test //...
```

To render an SVG as an image, try the renderer tool:
```
bazel run //src/svg/renderer:renderer_tool -- src/svg/renderer/testdata/Ghostscript_Tiger.svg
```

See more details in the [Building Donner](internal/building.md) instructions.
