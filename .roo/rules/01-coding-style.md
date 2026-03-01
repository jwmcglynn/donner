# Donner SVG Coding Style Summary

This document summarizes the main points from the full [Coding Style Guide](../../docs/coding_style.md).

## Core Principles

*   **Baseline:** Follows [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) with modifications for SVG naming conventions and C++20 features.
*   **Namespace:** All code resides within the `donner` namespace and its sub-namespaces (e.g., `donner::svg`).

## File Structure and Naming

*   **Folders:** `lower_snake_case`.
*   **Files:** `UpperCamelCase`, matching the primary class/struct (e.g., `SVGPathElement.h`).
    *   Source: `.cc`
    *   Headers: `.h`
    *   Tests: `_tests.cc`
*   **Headers:**
    *   Use `#pragma once`.
    *   Start with `/// @file`.

## Documentation (Doxygen)

*   Use `/** ... */` for classes, structs, methods.
*   Use `//!<` for brief inline member documentation.
*   Be comprehensive, explain context, usage, and limitations.
*   Use `\ref` for cross-referencing.

## Includes

*   Project files: `#include "donner/path/to/file.h"` (relative to Donner root).
*   System/Third-party: `#include <library/header.h>`.

## Symbol Naming

*   **Classes/Structs:** `UpperCamelCase`
*   **Methods:** `lowerCamelCase`
*   **Constructors/Static Factories:** `UpperCamelCase`
*   **Free Functions:** `UpperCamelCase`
*   **Member Variables:** `lowerCamelCaseWithTrailingUnderscore_`
*   **Parameters/Locals:** `lowerCamelCase`
*   **Constants:** `kUpperCamelCase`
*   **Units:** Include in names (e.g., `timeoutMs`) or use strong types.

## Formatting and Limits

*   **Column Limit:** 100 characters.
*   **Formatting:** Enforced by `.clang-format` in the repository root.

## Code Conventions

*   **Transforms:** Use `"destinationFromSource"` notation (e.g., `viewportFromLocal`).
*   **Const Correctness:** Apply `const` liberally to methods and variables.
*   **`struct` vs `class`:** `struct` for data aggregates, `class` for types with invariants/logic.
*   **Constructors:** Mark single-argument constructors `explicit` unless intentionally implicit (`/* implicit */`).
*   **Properties:** Use `thing()` / `setThing()`. Use `std::optional` for optional values.
*   **Enums:** Always use `enum class`. Provide `operator<<` for debugging.
*   **Asserts:**
    *   Debug: `assert(condition && "Message")`.
    *   Release: `UTILS_RELEASE_ASSERT(condition)` or `UTILS_RELEASE_ASSERT_MSG(condition, "Message")`.
*   **Strings:** Use `std::string_view` (non-owning), `RcString` (owning), `RcStringOrRef` (flexible API parameter). Use helpers from `"donner/base/StringUtils.h"`.
*   **`auto`:** Use sparingly, only when the type is obvious or for standard patterns (iterators, `ParseResult`).
*   **Operators:** Prefer `operator<=>` where possible (requires explicit `operator==` due to gtest bug). Provide `operator<<` for debugging.
*   **`constexpr`:** Use when possible.
*   **Preconditions:** Assert using `UTILS_RELEASE_ASSERT`.
*   **Language Restrictions:** Avoid user-defined literals and `long long`. Prefer `alignas(T)`
    byte buffers over `std::aligned_storage`/`std::aligned_union`.
