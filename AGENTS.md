# AGENT Instructions for Donner Repository

This repository contains the Donner SVG project written in modern C++20.

The bulk of the source lives in the `donner/` directory.
The `.roo/rules` directory provides condensed guidelines on coding style, architecture, and the rendering pipeline.

## Coding Style

- Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
  with project-specific conventions from `.roo/rules/01-coding-style.md`.
- Keep lines under **100 characters**. Formatting is enforced by `.clang-format` in the repo root.
- File/Folder naming:
  - Folders use `lower_snake_case`.
  - Files use `UpperCamelCase` (matching the main class), with `.cc` for sources,
    `.h` for headers and `_tests.cc` for tests.
- Headers start with `#pragma once` and a `/// @file` comment.
- Use Doxygen comments (`/** ... */`, `//!<`) for public APIs; all public methods,
  functions, and enumeration values must have doc comments. Prefer `///` for
  single-line comments, `/** */` blocks for multi-line comments, and `//!<` for
  short trailing snippets when they remain concise.
- Place all code in the `donner` namespace (and sub-namespaces like `donner::svg`).
- Never omit curly braces for control structures, even for single-line bodies.
- Naming conventions:
  - Classes/Structs: `UpperCamelCase`.
  - Methods: `lowerCamelCase`.
  - Member variables: `lowerCamelCaseWithTrailingUnderscore_`.
  - Constants: `kUpperCamelCase`.
- Use `std::string_view` for non-owning string parameters and prefer `constexpr` where possible.
- Apply `const` generously; mark single‑argument constructors `explicit` unless intended implicit.
- Use `enum class` and provide `operator<<` for debugging.
- Assert preconditions using `UTILS_RELEASE_ASSERT` or `assert` for debug builds.

## Architecture

- Donner builds a dynamic SVG **DOM** using an Entity‑Component‑System (ECS) with **EnTT**.
- Parsing, CSS, styling, layout, and rendering are separate components so they
  can be tested in isolation.
- Systems transform components through multiple stages (e.g., `StyleComponent`
  → `ComputedStyleComponent` → rendering instances).
- The rendering pipeline ultimately creates `RenderingInstanceComponent` objects
  consumed by a backend like Skia.

## General Practices

- Prefer existing Donner utilities (e.g., `Transformd`, `RcString`,
  `StringUtils`) before introducing new dependencies. Keep external libraries
  to a minimum and justify any additions.
- Optimize for readability and testability. Extract helpers rather than adding
  large inline logic blocks.
- Documentation: follow `docs/AGENTS.md` for writing user/developer docs, use templates under
  `docs/design_docs/`, keep lines under 100, and embed SVG/mermaid visuals where they clarify data
  flow or trust boundaries. Use Doxygen-friendly anchors `{#AnchorId}` and run `tools/doxygen.sh`
  when needed.
- Tests:
  - Use gMock with gTest for C++ tests.
  - Add fuzzers for parser-style code paths when practical.

## Building

- Both bazel and CMake are supported, but Bazel is the primary build system. CMake is experimental and may not support all features.
- The renderer is slow to build. Scope tests to specific directories when possible. Example: `bazel test //donner/base/...`.
- Use `bazel build //donner/...` to build everything.
- Run tests with `bazel test`.
- To build with CMake, run:
  ```bash
  python3 tools/cmake/gen_cmakelists.py && cmake -S . -B build && cmake --build build -j$(nproc)
  ```
  - CMake tests are also supported, but not built by default. To build tests, use:
  ```bash
  cmake -S . -B build -DDONNER_BUILD_TESTS=ON && cmake --build build -j$(nproc) && ctest --test-dir build
  ```

## Development Notes

- Format C++ code with `clang-format` and TypeScript/JSON/Markdown with `dprint`
  - Use `clang-format -i <files>` to reformat sources. `git clang-format` is handy to
  format only your pending changes. Avoid formatting files under `third_party/`
  (`dprint.json` sets line width to 100 and indent width to 2).
- Use `buildifier` to format Bazel build files (e.g. `.bzl`, `BUILD.bazel`).
  or `external/`.
- Design docs have dedicated guidance under `docs/design_docs/AGENTS.md`.
  Refer to those instructions when authoring or updating design documentation.
- For doc-only changes, don't run `clang-format` or `dprint`, or build.
- Doc files under docs/ are used to generate Doxygen documentation.
  - Use `tools/doxygen.sh` to generate the docs.
  - The generated docs are in `generated-doxygen/html/`.
- Use `tools/coverage.sh` to generate code coverage reports (if lcov is installed).
