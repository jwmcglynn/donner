# AGENT Instructions for Donner Repository

This repository contains the Donner SVG project written in modern C++20.
The bulk of the source lives in the `donner/` directory.
The `.roo/rules` directory provides condensed guidelines on coding style,
architecture, and the rendering pipeline.

## Coding Style

- Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
  with project-specific conventions from `.roo/rules/01-coding-style.md`.
- Keep lines under **100 characters**. Formatting is enforced by `.clang-format` in the repo root.
- File/Folder naming:
  - Folders use `lower_snake_case`.
  - Files use `UpperCamelCase` (matching the main class), with `.cc` for sources,
    `.h` for headers and `_tests.cc` for tests.
- Headers start with `#pragma once` and a `/// @file` comment.
- Use Doxygen comments (`/** ... */`, `//!<`) for public APIs.
- Place all code in the `donner` namespace (and sub-namespaces like `donner::svg`).
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

## Development Notes
- Format C++ code with `clang-format` and TypeScript/JSON/Markdown with `dprint`
  (`dprint.json` sets line width to 100 and indent width to 2).
- Use `clang-format -i <files>` to reformat sources. `git clang-format` is handy to
  format only your pending changes. Avoid formatting files under `third_party/`
  or `external/`.
- Tests are run with **Bazel**. Typical command:
  ```sh
  bazel test //donner/...
  ```
