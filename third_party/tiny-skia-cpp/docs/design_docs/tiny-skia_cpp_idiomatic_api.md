# Design: Idiomatic C++ Public API

**Status:** Implemented
**Author:** Claude
**Created:** 2026-03-04

## Summary

The public API currently follows Rust conventions — free functions like
`fillRect(pixmapMut, rect, paint, transform)` in the `tiny_skia` namespace.
This is unnatural in C++, where the same operations belong as methods on a
class.

This design:
1. Renames `PixmapRef` → `PixmapView` and `PixmapMut` → `MutablePixmapView`.
2. Moves the free drawing functions into a `Painter` class as static methods.
3. Adds convenience instance methods on `Pixmap` and `MutablePixmapView` that
   delegate to `Painter`.
4. Hides implementation-detail symbols from the public surface.
5. Adds `Path::fromRect()` and `Path::fromCircle()` convenience statics.

All changes are zero-overhead: no allocations, no virtual dispatch.

## Goals

- Rename Rust-flavored view types: `PixmapRef` → `PixmapView`,
  `PixmapMut` → `MutablePixmapView`, `SubPixmapMut` → `MutableSubPixmapView`.
- Free drawing functions become `Painter::fillRect(...)` etc. (static methods).
- `Pixmap`/`MutablePixmapView` get instance methods as syntactic sugar.
- Default `Transform::identity()` parameter eliminates boilerplate.
- Internal helpers (`DrawTiler`, `isTooBigForMath`, `treatAsHairline`,
  `strokeHairline`) move into `namespace detail` or become private.
- Add `Path::fromRect()` and `Path::fromCircle()` convenience statics.
- Replace `dataMut()`/`pixelsMut()` with const/non-const overloads.
- Adopt STL naming: `len()` → `size()`, `isEmpty()` → `empty()`,
  `take()` → `release()`.
- Fix abbreviations: `lengthSqd()` → `lengthSquared()`, etc.
- Consistent casing: `fromXywh()` → `fromXYWH()`, `fromLtrb()` → `fromLTRB()`.
- Drop Rust `as_` prefix: `asSubmask()` → `submask()`.
- `PathBuilder` methods return `PathBuilder&` for fluent chaining.
- Zero new allocations, zero virtual calls, zero overhead vs current code.

## Non-Goals

- Changing internal implementation or rendering pipeline.
- Wrapping `Shader` (the `std::variant` alias) in a class — the implicit
  conversion from `Color` already works well.
- Adding builder/fluent patterns to `Stroke` or `Paint` — their public
  aggregate fields are idiomatic C++ already.
- Renaming `from*()` factory methods — named constructors are idiomatic C++.
- Renaming `is*()` predicates — standard C++ naming.
- Replacing `std::optional` returns — this is modern C++ best practice.
- Renaming `preConcat()`/`postConcat()` — standard matrix terminology.

## Next Steps

1. Rename `PixmapRef` → `PixmapView`, `PixmapMut` → `MutablePixmapView`,
   `MutableSubPixmapView` → `MutableSubPixmapView`.
2. Introduce `Painter` class with static methods in `Painter.h`.
3. Move internal helpers into `Painter` as private statics or `detail`.
4. Add drawing methods to `Pixmap` and `MutablePixmapView`.
5. Add `Path::fromRect()` / `Path::fromCircle()`.
6. Build and test.

## Implementation Plan

- [x] Milestone 0: Rename view types
  - [x] Rename `PixmapView` → `PixmapView` throughout
  - [x] Rename `PixmapMut` → `MutablePixmapView` throughout
  - [x] Rename `MutableSubPixmapView` → `MutableSubPixmapView` throughout
  - [x] Rename `SubMaskView` → `MaskView` or `SubMaskView` if applicable
  - [x] Update `Pixmap::asRef()` → `Pixmap::view()`,
        `Pixmap::asMut()` → `Pixmap::mutableView()`
  - [x] Build and test gate
- [x] Milestone 1: `Painter` class with static methods
  - [x] Wrap existing free functions as `Painter::fillRect`,
        `Painter::fillPath`, `Painter::strokePath`, `Painter::drawPixmap`,
        `Painter::applyMask` — all `static`
  - [x] Move `DrawTiler`, `isTooBigForMath`, `treatAsHairline`,
        `strokeHairline` into `Painter` as `private` statics
        (or a `detail` namespace)
  - [x] Move `Paint` and `PixmapPaint` out of `Painter.h` into their own
        header `Paint.h` (they are value types, not painter internals)
  - [x] Remove old free functions entirely; migrate all call sites (~180)
        to `Painter::*`
  - [x] Add default `Transform` parameter:
        `Transform transform = Transform::identity()`
  - [x] Build and test gate
- [x] Milestone 2: Instance methods on `Pixmap` and `MutablePixmapView`
  - [x] Add drawing method declarations to `Pixmap` in `Pixmap.h`
  - [x] Add drawing method declarations to `MutablePixmapView` in `Pixmap.h`
  - [x] Implement in `Pixmap.cpp` (thin delegation to `Painter::*`)
  - [x] Build and test gate
- [x] Milestone 3: Path convenience factories
  - [x] Add `Path::fromRect(const Rect&)` static method
  - [x] Add `Path::fromCircle(float cx, float cy, float radius)` static
  - [x] Remove `pathFromRect()` free function; migrate all call sites
  - [x] Build and test gate
- [x] Milestone 4: Naming cleanup (STL conventions and abbreviations)
  - [x] `dataMut()` → const/non-const overloads of `data()`;
        `pixelsMut()` → const/non-const overloads of `pixels()`
        (Pixmap, MutablePixmapView, Mask)
  - [x] `len()` → `size()` (Path, PathBuilder — STL convention)
  - [x] `isEmpty()` → `empty()` (Path, PathBuilder — STL convention)
  - [x] `take()` → `release()` (Pixmap, Mask — matches `unique_ptr`)
  - [x] `takeDemultiplied()` → `releaseDemultiplied()` (Pixmap)
  - [x] `lengthSqd()` → `lengthSquared()`,
        `distanceToSqd()` → `distanceToSquared()` (Point)
  - [x] `rotateCw()` → `rotateClockwise()`,
        `rotateCcw()` → `rotateCounterClockwise()` (Point)
  - [x] `fromXywh()` → `fromXYWH()` (Rect — consistent casing)
  - [x] `fromLtrb()` → `fromLTRB()` (Rect — consistent casing)
  - [x] `fromWh()` → `fromWH()` (IntSize)
  - [x] `fromXy()` → `fromXY()` (Point)
  - [x] `asSubmask()` → `submask()`, `asSubpixmap()` → `subpixmap()`
        (Mask — drop Rust `as_` prefix)
  - [x] Build and test gate
- [x] Milestone 5: Builder fluent return (PathBuilder)
  - [x] `moveTo()`, `lineTo()`, `quadTo()`, `cubicTo()`, `close()`
        return `PathBuilder&` instead of `void` (enables chaining)
  - [x] Build and test gate

## Proposed Architecture

### Before (Rust-style)

```cpp
#include "tiny_skia/Painter.h"

auto pixmap = Pixmap::fromSize(100, 100);
auto mut = pixmap->mutableView();

Paint paint;
paint.setColorRgba8(0, 128, 255, 255);

auto rect = Rect::fromXywh(10, 10, 80, 80);
fillRect(mut, *rect, paint, Transform::identity());

auto path = PathBuilder::fromCircle(50, 50, 40);
fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
```

### After (idiomatic C++)

```cpp
#include "tiny_skia/Painter.h"

auto pixmap = Pixmap::fromSize(100, 100);

Paint paint;
paint.setColorRgba8(0, 128, 255, 255);

// Option A: Static methods on Painter (explicit about which subsystem)
auto rect = Rect::fromXywh(10, 10, 80, 80);
Painter::fillRect(pixmap->mutableView(), *rect, paint);

auto path = Path::fromCircle(50, 50, 40);
Painter::fillPath(pixmap->mutableView(), *path, paint, FillRule::Winding);

// Option B: Instance methods on Pixmap (most concise)
pixmap->fillRect(*rect, paint);
pixmap->fillPath(*path, paint, FillRule::Winding);
```

Key differences:
- `Painter::fillRect(...)` groups drawing operations under a class
- `pixmap->fillRect(...)` is available as sugar for the common case
- No `Transform::identity()` boilerplate (defaulted)
- `Path::fromCircle` instead of `PathBuilder::fromCircle`

### Painter class

```cpp
/// Groups all drawing operations. All methods are static — Painter has no
/// state and cannot be instantiated. This replaces the previous free
/// functions and provides a clear public API surface.
class Painter {
 public:
  Painter() = delete;

  /// Fills an axis-aligned rectangle onto the pixmap.
  static void fillRect(MutablePixmapView& pixmap, const Rect& rect,
                       const Paint& paint,
                       Transform transform = Transform::identity(),
                       const Mask* mask = nullptr);

  /// Fills a path onto the pixmap.
  static void fillPath(MutablePixmapView& pixmap, const Path& path,
                       const Paint& paint, FillRule fillRule,
                       Transform transform = Transform::identity(),
                       const Mask* mask = nullptr);

  /// Strokes a path onto the pixmap.
  static void strokePath(MutablePixmapView& pixmap, const Path& path,
                         const Paint& paint, const Stroke& stroke,
                         Transform transform = Transform::identity(),
                         const Mask* mask = nullptr);

  /// Composites a source pixmap onto a destination pixmap.
  static void drawPixmap(MutablePixmapView& pixmap, std::int32_t x,
                         std::int32_t y, PixmapView src,
                         const PixmapPaint& paint = {},
                         Transform transform = Transform::identity(),
                         const Mask* mask = nullptr);

  /// Applies a mask to already-drawn content.
  static void applyMask(MutablePixmapView& pixmap, const Mask& mask);

 private:
  // Internal helpers — no longer on the public surface.
  static bool isTooBigForMath(const Path& path);
  static std::optional<float> treatAsHairline(const Paint& paint,
                                              float strokeWidth,
                                              Transform ts);
  static void strokeHairline(const Path& path, const Paint& paint,
                             LineCap lineCap,
                             std::optional<SubMaskView> mask,
                             SubMutablePixmapView& subpix);
};
```

`DrawTiler` moves into `namespace detail` (it's a multi-line class, awkward
as a private nested class, and tests may want to unit-test it):

```cpp
namespace detail {
class DrawTiler { /* unchanged implementation */ };
}  // namespace detail
```

### Paint extraction

`Paint` and `PixmapPaint` are currently defined in `Painter.h`. They are
standalone value types with no dependency on `Painter`, so they move to a
new `Paint.h` header. `Painter.h` includes `Paint.h`. Existing code that
includes `Painter.h` continues to compile unchanged.

```
// New file: Paint.h
struct Paint { ... };      // moved from Painter.h
struct PixmapPaint { ... }; // if it exists, also moved
```

### Pixmap / MutablePixmapView instance methods

```cpp
class Pixmap {
 public:
  // ... existing API unchanged ...

  void fillRect(const Rect& rect, const Paint& paint,
                Transform transform = Transform::identity(),
                const Mask* mask = nullptr);

  void fillPath(const Path& path, const Paint& paint,
                FillRule fillRule,
                Transform transform = Transform::identity(),
                const Mask* mask = nullptr);

  void strokePath(const Path& path, const Paint& paint,
                  const Stroke& stroke,
                  Transform transform = Transform::identity(),
                  const Mask* mask = nullptr);

  void drawPixmap(std::int32_t x, std::int32_t y, PixmapView src,
                  const PixmapPaint& paint = {},
                  Transform transform = Transform::identity(),
                  const Mask* mask = nullptr);

  void applyMask(const Mask& mask);
};
```

`MutablePixmapView` gets the same set.

### Implementation (all thin wrappers)

```cpp
// Pixmap delegates through mutableView() → Painter
void Pixmap::fillRect(const Rect& rect, const Paint& paint,
                      Transform transform, const Mask* mask) {
  auto view = mutableView();
  Painter::fillRect(view, rect, paint, transform, mask);
}

// MutablePixmapView delegates directly to Painter
void MutablePixmapView::fillRect(const Rect& rect, const Paint& paint,
                         Transform transform, const Mask* mask) {
  Painter::fillRect(*this, rect, paint, transform, mask);
}
```

### Call-site migration

All ~180 call sites are updated in the same change. No deprecated wrappers.
The free functions (`fillRect`, `fillPath`, `strokePath`, `drawPixmap`,
`applyMask`, `strokeHairline`, `pathFromRect`) are removed entirely.

Migration patterns:

| Before | After |
|--------|-------|
| `fillRect(mut, *rect, paint, Transform::identity())` | `Painter::fillRect(mut, *rect, paint)` |
| `fillPath(mut, path, paint, FillRule::Winding, ts)` | `Painter::fillPath(mut, path, paint, FillRule::Winding, ts)` |
| `strokePath(mut, path, paint, stroke, ts)` | `Painter::strokePath(mut, path, paint, stroke, ts)` |
| `drawPixmap(mut, x, y, src, ppaint, ts)` | `Painter::drawPixmap(mut, x, y, src, ppaint, ts)` |
| `applyMask(mut, mask)` | `Painter::applyMask(mut, mask)` |
| `pathFromRect(*rect)` | `Path::fromRect(*rect)` |

Where `Transform::identity()` was the only transform argument, it can now
be omitted entirely thanks to the default parameter.

### Path convenience factories

```cpp
class Path {
 public:
  /// Creates a rectangular path.
  static Path fromRect(const Rect& rect);

  /// Creates a circular path. Returns nullopt for non-positive radius.
  static std::optional<Path> fromCircle(float cx, float cy, float r);
};
```

`Path::fromRect` replaces the free function `pathFromRect()`.
`Path::fromCircle` delegates to `PathBuilder::fromCircle()`.

### Circular dependency note

`Pixmap.h` currently does not include `Painter.h`. The new drawing methods
on `Pixmap`/`MutablePixmapView` need `Paint`, `Path`, `Stroke`, `Mask`, etc.

**Approach: Forward-declare in `Pixmap.h`, implement in `Pixmap.cpp`.**
The method bodies go in `Pixmap.cpp` which includes `Painter.h`. `Pixmap.h`
only needs forward declarations for `Paint`, `PixmapPaint`, `Path`, `Stroke`,
`Mask`, `Rect`, `FillRule`, `Transform`. This avoids circular includes.

### Rename mapping

| Before (Rust-flavored) | After (C++ idiomatic) |
|------------------------|-----------------------|
| `PixmapRef` | `PixmapView` |
| `PixmapMut` | `MutablePixmapView` |
| `SubPixmapMut` | `MutableSubPixmapView` |
| `SubMaskRef` | `SubMaskView` |
| `Pixmap::asRef()` | `Pixmap::view()` |
| `Pixmap::asMut()` | `Pixmap::mutableView()` |

Both view types keep two types (not unified) to preserve compile-time
const-safety: `PixmapView` holds `const uint8_t*`, `MutablePixmapView`
holds `uint8_t*`.

### Naming cleanup (Milestone 4)

| Before | After | Files affected |
|--------|-------|----------------|
| `dataMut()` | `data()` (non-const overload) | Pixmap.h, Mask.h |
| `pixelsMut()` | `pixels()` (non-const overload) | Pixmap.h |
| `len()` | `size()` | Path.h, PathBuilder.h |
| `isEmpty()` | `empty()` | Path.h, PathBuilder.h |
| `take()` | `release()` | Pixmap.h, Mask.h |
| `takeDemultiplied()` | `releaseDemultiplied()` | Pixmap.h |
| `lengthSqd()` | `lengthSquared()` | Point.h |
| `distanceToSqd()` | `distanceToSquared()` | Point.h |
| `rotateCw()` | `rotateClockwise()` | Point.h |
| `rotateCcw()` | `rotateCounterClockwise()` | Point.h |
| `fromXywh()` | `fromXYWH()` | Geom.h (Rect) |
| `fromLtrb()` | `fromLTRB()` | Geom.h (Rect) |
| `fromWh()` | `fromWH()` | Geom.h (IntSize) |
| `fromXy()` | `fromXY()` | Point.h |
| `asSubmask()` | `submask()` | Mask.h |
| `asSubpixmap()` | `subpixmap()` | Mask.h, Pixmap.h |

### Fluent builder (Milestone 5)

```cpp
// Before: void returns
PathBuilder b;
b.moveTo(0, 0);
b.lineTo(10, 10);
b.close();
auto path = b.finish();

// After: chaining via PathBuilder& returns
auto path = PathBuilder()
    .moveTo(0, 0)
    .lineTo(10, 10)
    .close()
    .finish();
```

## File changes summary

| File | Change |
|------|--------|
| `Paint.h` (new) | `Paint`, `PixmapPaint` extracted from `Painter.h` |
| `Painter.h` | `Painter` class with static methods; `detail::DrawTiler`; no free functions; includes `Paint.h` |
| `Painter.cpp` | Rename free functions to `Painter::` static methods |
| `Pixmap.h` | Rename types, forward declarations, drawing methods, `dataMut()` → overloads, `take()` → `release()` |
| `Pixmap.cpp` | Drawing method implementations delegating to `Painter` |
| `Path.h` | Add `Path::fromRect()`, `Path::fromCircle()`; `len()` → `size()`, `isEmpty()` → `empty()` |
| `Path.cpp` | Implement the new statics |
| `PathBuilder.h` | `len()` → `size()`, `isEmpty()` → `empty()`; return `PathBuilder&` for chaining |
| `PathBuilder.cpp` | Update return types to `PathBuilder&` |
| `Point.h` | `lengthSqd()` → `lengthSquared()`, `rotateCw()` → `rotateClockwise()`, etc. |
| `Geom.h` | `fromXywh()` → `fromXYWH()`, `fromLtrb()` → `fromLTRB()`, `fromWh()` → `fromWH()` |
| `Mask.h` | Rename types, `dataMut()` → overloads, `take()` → `release()`, `asSubmask()` → `submask()` |

## Testing and Validation

- All existing tests updated to use `Painter::*` methods.
- Add a small set of tests exercising `pixmap.fillRect(...)` instance
  methods to verify delegation.
- No golden-image changes expected since behavior is identical.
- Build and test gate: `bazel build //...` and `bazel test //...`.

## Alternatives Considered

**Free functions only (status quo):**
Works, but is a Rust-ism. Doesn't group related operations and pollutes
the namespace. No discoverability via IDE autocomplete on the class.

**Standalone `Canvas` class wrapping `MutablePixmapView`:**
A non-owning wrapper like `Canvas(pixmapMut)` with drawing methods. Rejected
because `Painter` with static methods achieves the same grouping without
requiring construction of a wrapper object. Instance methods on `Pixmap`
cover the convenience case.

**Replace `PixmapView`/`MutablePixmapView` with `const Pixmap&`/`Pixmap&`:**
Would break users who create views into external memory (e.g., from a
windowing toolkit's framebuffer). Keep the existing types.

**Wrapping `Shader` variant in a class:**
The `std::variant<Color, LinearGradient, ...>` works well with implicit
conversion from `Color`. Wrapping it adds complexity for little gain.
