# Design: Code Readability Review and C++20 Hardening

**Status:** In Progress
**Author:** Claude
**Created:** 2026-03-04
**Updated:** 2026-03-04

## Summary

Module-by-module readability review of the entire C++ codebase against Google C++ style and modern
C++20 idioms. Covers naming, const-correctness, `[[nodiscard]]`, porting-artifact cleanup, and
minor code-quality improvements — all behavior-preserving.

## Goals

- Normalize naming to Google C++ style (lowerCamelCase members, `kCamelCase` constants).
- Remove intermediate porting comments ("Matches Rust", "Rust original", etc.).
- Add `[[nodiscard]]` where discarding a return value is a bug.
- Fix isolated style violations (malformed comments, redundant casts, copy-by-value on arrays).

## Non-Goals

- Behavior changes, algorithmic rewrites, or new features.
- Rewriting stable modules (wide/SIMD backends, core math).
- Doxygen or API doc additions (tracked separately in Milestone 4 of the roadmap).

## Next Steps

1. Fix all high-priority findings.
2. Build and test (`bazel build //...` && `bazel test //...`).
3. Commit, then proceed to medium-priority findings.

## Implementation Plan

- [x] Milestone 1: High-Priority Fixes
  - [x] Rename `Stroke.h` members: `miter_limit` → `miterLimit`, `line_cap` → `lineCap`,
    `line_join` → `lineJoin`, and all usages.
  - [x] Rename `FocalData` members: `focal_x` → `focalX`, `is_swapped` → `isSwapped`,
    and all usages.
  - [x] Normalize `realWidth` vs `real_width` to `realWidth` across `SubMaskRef`, `SubMaskMut`,
    `SubPixmapMut`, and pipeline `MaskCtx`. (Already consistent — verified.)
  - [x] Remove ~96 "Matches Rust" / "Rust original" porting-artifact comments from source files
    (excludes copyright headers and intentional Rust-parity notes in design docs).
    6 intentional references retained (FMA pragma, precision notes, test context).
  - [x] Add `[[nodiscard]]` to `Pixmap::take()`, `Pixmap::takeDemultiplied()`, and `Mask::take()`.
  - [x] Fix malformed comment `/ blend_fn:` → verified false positive; comment is correct.
  - [ ] Build and test gate. (Bazel registry unreachable in current environment.)
- [x] Milestone 2: Medium-Priority Fixes
  - [x] Change array parameters to `const&` in `PathGeometry.h` (10 functions + wrappers).
  - [x] Rename `QUAD_RECURSIVE_LIMIT` → `kQuadRecursiveLimit` in `Stroker.cpp`.
  - [x] Rename `data_len` → `dataLen` in `Mask.cpp`.
  - [x] Normalize pipeline/Paint member naming to camelCase throughout.
  - [ ] Build and test gate.
- [ ] Milestone 3: Low-Priority Fixes
  - [ ] Add `[[nodiscard]]` to internal scan-module helpers.
  - [ ] Remove redundant/obvious comments (case-by-case).
  - [ ] Build and test gate.

## Proposed Architecture

No architectural changes. All edits are rename/comment/attribute changes verified by build+test.

## Review Findings by Module

### Core (Geom, Point, Scalar, Math, Color, Transform, FloatingPoint, F32x2)
**Verdict: Clean.** Good const-correctness, proper `[[nodiscard]]`, consistent naming.

### Path (Path, PathBuilder, PathGeometry, PathVec, Edge*, Stroker, Dash)
- `PathGeometry.h`: 8+ functions take `std::array<Point, N>` by value instead of `const&`.
- `Stroker.cpp:26`: `QUAD_RECURSIVE_LIMIT` uses UPPER_SNAKE not `kCamelCase`.
- ~~Scattered "Matches Rust" comments.~~ Removed.

### Pipeline (Blitter, Highp, Lowp, Mod)
- ~~`MaskCtx::real_width`~~ — already `realWidth`, consistent.
- ~~`Lowp.cpp:287`: malformed comment~~ — verified correct; false positive.
- Stage function names use snake_case (`move_source_to_destination`); intentional match to
  enum-driven dispatch — not renamed.

### Scan (Path, Hairline, HairlineAa, PathAa)
- 25+ internal helper functions missing `[[nodiscard]]`.
- ~~Scattered "Matches Rust" comments.~~ Removed.

### Shaders (Gradient, Linear, Radial, Sweep, Pattern)
- ~~`FocalData::focal_x`, `is_swapped`~~ — renamed to camelCase (`focalX`, `isSwapped`).
- ~~Porting comments.~~ Removed.

### Wide/SIMD (F32x4T, F32x8T, I32x4T, U16x16T, U32x4T, U32x8T, backends)
**Verdict: Very clean.** No issues found.

### Top-Level (Painter, Pixmap, Stroker, Blitter, Mask)
- ~~`Stroke::miter_limit`, `line_cap`, `line_join`~~ — renamed to camelCase.
- ~~`SubPixmapMut::real_width` vs `SubMaskRef::realWidth`~~ — already consistent (`realWidth`).
- ~~`Pixmap::take()`, `takeDemultiplied()` missing `[[nodiscard]]`~~ — added.

## Testing and Validation

- `bazel build //...` and `bazel test //...` after each milestone.
- All changes are rename/comment/attribute only — no behavior changes.
