# Donner 🌩

[![Build Status](https://github.com/jwmcglynn/donner/actions/workflows/main.yml/badge.svg)](https://github.com/jwmcglynn/donner/actions/workflows/main.yml) [![License: ISC](https://img.shields.io/badge/License-ISC-blue.svg)](https://opensource.org/licenses/ISC) [![codecov](https://codecov.io/gh/jwmcglynn/donner/branch/main/graph/badge.svg?token=Z3YJZNKGU0)](https://codecov.io/gh/jwmcglynn/donner) ![loc](https://gist.githubusercontent.com/jwmcglynn/91f7f490a72af9c06506c8176729d218/raw/loc.svg)
![comments](https://gist.githubusercontent.com/jwmcglynn/91f7f490a72af9c06506c8176729d218/raw/comments.svg)

Donner is an under-development modern C++20 SVG rendering library which provides full access to the SVG DOM, enabling browser-level functionality without the browser.

![Donner splash image](donner_splash.svg)

## TODO

- [x] [Build reports](docs/build_report.md), https://github.com/jwmcglynn/donner/issues/145
- [ ] Documentation for current status, https://github.com/jwmcglynn/donner/issues/149
- [ ] Documentation for how to use
- [x] Documentation for how to build
- [ ] Performance benchmark
- [ ] Introduction docs that references the above

Currently, Donner includes:

- SVG2 core functionality, such as shapes, fills, strokes, and gradients.
- CSS3 parsing and cascading support, with a hand-rolled library.
- An [EnTT](https://github.com/skypjack/entt) ECS-backed document tree.
- A SVG DOM-style API to traverse, inspect, and modify documents in memory.
- A two-phase renderer, which builds and caches a rendering tree for efficient frame-based rendering.

Donner currently renders with Skia as core functionality is being implemented. While Skia is powerful, it adds a lot of code size so alternative approaches may be considered in the future.

Donner focuses on security and performance, which is validated with code coverage and fuzz testing.

## Project Goals

- Have minimal dependencies, so it can be integrated into existing applications, assuming a modern compiler.
- Expose the SVG DOM, so that applications can manipulate SVGs dynamically.
- Implement the [SVG 2 Specification](https://www.w3.org/TR/SVG2/).

## Documentation

- [Getting started](docs/getting_started.md)
- [About Donner](docs/about_donner.md)
- [API Documentation](https://jwmcglynn.github.io/donner/namespaces.html)
- [System Architecture](docs/architecture.md)
- [Building Donner](docs/internal/building.md)

## Other Libraries

- **[librsvg](https://gitlab.gnome.org/GNOME/librsvg)**: Provides a simple way to render SVGs one-shot, does not provide a DOM or animation
- **[resvg](https://github.com/RazrFalcon/resvg)**: A Rust library that focuses on correctness, safety, and portability for static SVGs
