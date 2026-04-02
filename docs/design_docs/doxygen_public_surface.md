# Design: Doxygen Documentation & Public Surface

**Status:** Complete
**Author:** Claude
**Created:** 2026-03-04

## Summary

Add concise Doxygen documentation to the public API headers, establish a clear
public/private boundary, and add CI infrastructure to build and deploy docs to
GitHub Pages.

## Goals

- Concise doc comments optimized for LLM consumption (no boilerplate, no
  redundant `@param` for self-evident parameters).
- Clear public surface: users include `<tiny_skia/Foo.h>` for public types.
  Internal headers are excluded from generated docs.
- `EXTRACT_PRIVATE = NO` so private members/classes are hidden from users.
- GitHub Actions workflow to build Doxygen and deploy to GitHub Pages on push
  to `main`.
- Local builds via `doxygen Doxyfile` from repo root.

## Non-Goals

- Exhaustive per-parameter documentation (concise > verbose for LLMs).
- Documenting internal namespaces (`detail`, `scan`, `pipeline::highp`, etc.).
- Tutorial-style documentation (this is API reference only).

## Public Headers

These headers form the documented public surface:

| Header | Contents |
|--------|----------|
| `Painter.h` | `Painter` (static drawing methods), `detail::DrawTiler` excluded |
| `Paint.h` | `Paint`, `PixmapPaint` (in shaders/Pattern.h) |
| `Pixmap.h` | `Pixmap`, `PixmapView`, `MutablePixmapView`, `MutableSubPixmapView` |
| `Mask.h` | `Mask`, `SubMaskView`, `MutableSubMaskView` |
| `Path.h` | `Path`, `PathSegment`, `FillRule`, `PathVerb`, `LineCap` |
| `PathBuilder.h` | `PathBuilder` |
| `Color.h` | `Color`, `ColorU8`, `PremultipliedColorU8`, `PremultipliedColor`, `ColorSpace` |
| `Geom.h` | `Rect`, `IntRect`, `ScreenIntRect`, `IntSize`, `Point` |
| `Point.h` | `Point` (already in Geom.h forward, full definition) |
| `Transform.h` | `Transform` |
| `Stroke.h` | `Stroke`, `StrokeDash` |
| `Dash.h` | `StrokeDash` |
| `BlendMode.h` | `BlendMode` enum |
| `shaders/Mod.h` | `Shader` variant alias |
| `shaders/LinearGradient.h` | `LinearGradient` |
| `shaders/RadialGradient.h` | `RadialGradient` |
| `shaders/SweepGradient.h` | `SweepGradient` |
| `shaders/Pattern.h` | `Pattern`, `PixmapPaint` |

## Internal Headers (excluded from docs)

Everything not listed above, including:
- `AlphaRuns.h`, `Blitter.h`, `Edge.h`, `EdgeBuilder.h`, `EdgeClipper.h`
- `LineClipper.h`, `PathGeometry*.h`, `PathSegmentsIter` (in Path.h but internal)
- `FixedPoint.h`, `FloatingPoint.h`, `Math.h`, `Scalar.h`, `F32x2.h`, `F32x4.h`
- `pipeline/*` (except `pipeline/Mod.h` for `SpreadMode` enum)
- `scan/*`, `path64/*`, `wide/*`
- `MaskOps.cpp`, `Stroker.cpp` (implementation files, not headers)

Strategy: use `@internal` on non-public classes that live in public headers
(e.g. `PathSegmentsIter`). Use Doxygen `EXCLUDE_PATTERNS` for entire
internal subdirectories.

## Implementation Plan

- [x] Milestone 1: Doxyfile setup
  - [x] Create `Doxyfile` with public-headers-only INPUT list
  - [x] Configure: `EXTRACT_PRIVATE = NO`, `EXTRACT_STATIC = YES`,
        `HIDE_UNDOC_MEMBERS = YES`, `HIDE_UNDOC_CLASSES = YES`
  - [x] `INPUT` = public headers only (explicit list)
  - [x] Add `docs/mainpage.md` as main page
  - [x] Verify local build produces clean output (zero warnings)

- [x] Milestone 2: Add doc comments to public headers
  - [x] `Painter.h` — class + each static method (1 line each)
  - [x] `Paint.h` — struct fields + methods
  - [x] `Pixmap.h` — all 4 classes, factory methods, view accessors
  - [x] `Mask.h` — class + factories + fillPath/intersectPath
  - [x] `Path.h` — class + factories + transform/stroke/dash
  - [x] `PathBuilder.h` — class + all builder methods
  - [x] `Color.h` — all 4 color classes + ColorSpace enum
  - [x] `Geom.h` — Rect, IntRect, ScreenIntRect, IntSize
  - [x] `Point.h` — Point struct
  - [x] `Transform.h` — class + factories + operations
  - [x] `Stroke.h` — struct + fields
  - [x] `BlendMode.h` — enum values
  - [x] `shaders/*.h` — gradient + pattern factories
  - [x] Mark internal symbols with `@internal`

- [x] Milestone 3: GitHub Pages deployment workflow
  - [x] Create `.github/workflows/docs.yml`
  - [x] Install doxygen in CI
  - [x] Build docs
  - [x] Deploy to gh-pages branch using `actions/deploy-pages`

## Doc Comment Style Guide

Optimized for conciseness and LLM consumption:

```cpp
/// Fills an axis-aligned rectangle.
static void fillRect(MutablePixmapView& pixmap, const Rect& rect,
                     const Paint& paint,
                     Transform transform = Transform::identity(),
                     const Mask* mask = nullptr);
```

Rules:
1. One `///` line for simple methods. Multi-line only when truly needed.
2. No `@param` for self-evident parameters (paint, transform, mask).
3. Use `@param` only when behavior is non-obvious.
4. `@return` only when not obvious from signature.
5. No `@brief` — first sentence IS the brief.
6. No `@see` cross-references (noise for LLMs).
7. Document nullopt/optional return semantics inline:
   `/// Returns nullopt for non-positive radius.`
