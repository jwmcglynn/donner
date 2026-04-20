# Donner

## Unreleased

### Removed

- Full-Skia renderer backend removed. `tiny-skia` remains the default backend and Geode remains available.

## v0.5.0

Major release adding a software rendering backend, complete text rendering, all 17 SVG filter
primitives, and a CLI tool — while maintaining the existing Skia backend.

### Highlights

- **`<text>` support** — Full `<text>`, `<tspan>`, and `<textPath>` support with per-character
  positioning, text-anchor, text-decoration, and all baseline properties. Fonts are loaded from
  `@font-face` rules (TTF, OTF, WOFF, WOFF2). An optional HarfBuzz tier (`--config=text-full`)
  adds complex script shaping.
- **`<filter>` support — all 17 SVG filter primitives** — feGaussianBlur, feColorMatrix,
  feComposite, feBlend, feTurbulence, feConvolveMatrix, feDiffuseLighting, feSpecularLighting,
  feDisplacementMap, feComponentTransfer, feMorphology, feFlood, feOffset, feMerge, feTile,
  feImage, feDropShadow. Float-precision pipeline with SIMD (NEON) optimizations. CSS shorthand
  filter functions (`blur()`, `brightness()`, `drop-shadow()`, etc.) also supported.
- **tiny-skia software renderer** — A new default rendering backend written in C++ that requires no
  external GPU libraries. Achieves performance within 1.5x of Skia across all rendering operations.
  The Skia backend remains available as a build option. A vectorized WASM SIMD128 backend ships
  alongside the existing ARM NEON / x86 AVX2 / scalar paths and is selected automatically when
  Donner is compiled with Emscripten's `-msimd128`.
- **`donner-svg` CLI tool** — Render SVGs to PNG, preview in the terminal, or interactively
  inspect elements with mouse selection.
- **WebAssembly build** — Donner's renderer and parser now build through the Emscripten toolchain
  via `bazel build --config=wasm //...`, with an `examples/svg_viewer` demo that parses and
  renders SVGs entirely in the browser using ImGui + emscripten-glfw.
- **Donner Editor** — A new in-tree SVG editor lives under `donner/editor/`, shipping both as a
  native desktop application and as a WebAssembly edition that runs entirely in the browser.
- **`.svgz` support** — `SVGParser` auto-detects the gzip magic bytes and decompresses `.svgz`
  input transparently, so gzipped SVG files load without any extra ceremony at the call site.

### What's Changed

- **Renderer architecture** — Abstract `RendererInterface` with `RendererDriver` traversal,
  enabling backend-agnostic rendering. Both Skia and tiny-skia implement the same interface.
- **Incremental invalidation** — Dirty flags on DOM mutations with O(1) fast path for unchanged
  documents. `StyleSystem` performs selective per-entity recomputation.
- **External SVG references** — `<image>` and `<use>` elements can reference external `.svg` files
  with sandboxed resource loading.
- **`<clipPath>` `<use>` support** — `<use>` children inside `<clipPath>` now resolve correctly.
- **`transform-origin` presentation attribute** — Accepted as both a presentation attribute
  and a CSS property, with the single-keyword grammar (`top`/`bottom`/`left`/`right`/`center`)
  parsed per the spec.
- **XML entity support** — Custom `<!ENTITY>` declarations in doctypes are now parsed by default.
- **XML parser conformance** — `Name` token validation per XML 1.0 spec.
- **CSS Color 4** — Full CSS Color 4 parsing plus `color-profile` aliases and modern color
  function grammar. Hex colors, `rgb()`/`rgba()`, `hsl()`/`hsla()`, `hwb()`, `lab()`, `lch()`,
  `oklab()`, `oklch()`, and `color()` all round-trip through the parser.
- **Unified parser diagnostics** — `ParseDiagnostic` replaces `ParseError`, carries a
  `DiagnosticSeverity` and half-open `SourceRange`, and routes non-fatal warnings through
  a lazy-evaluated `ParseWarningSink`. Clang/rustc-style source-context rendering via
  `DiagnosticRenderer`.
- **CMake support** — Full CMake build for both Skia and tiny-skia backends with feature toggles
  for text, WOFF2, and filters.
- **Fuzzing** — Continuous fuzzing harness with Docker support; all parser surfaces fuzz-tested.
- **81.7% line coverage** across production code (84.0% function coverage, 73.4% branch
  coverage).

### Fixes and Internals

- **`CompileTimeMap` 32-bit correctness fix** — The compile-time perfect-hash map used for CSS
  named colors, presentation attributes, and shape property tables was silently broken on
  32-bit targets: the splitmix64 finalizer performed a `>> 33` shift on `std::size_t`, which is
  undefined behavior when `size_t` is 32 bits. The hash has been reworked to use explicit
  `uint64_t` arithmetic. Native builds (where `size_t` is already 64-bit) are unaffected, but
  WebAssembly builds now produce correct lookups for every property map.
- **Modular EnTT headers** — Switched from the monolithic `entt.hpp` amalgamation to the
  modular EnTT headers, improving clean-build compile time and making incremental rebuilds
  after ECS-adjacent edits measurably cheaper.
- **resvg test suite upgraded** — Upgraded the vendored `resvg-test-suite` snapshot and
  reworked the skip mechanism to carry a reason string per skipped test, which made it
  straightforward to enable another wave of previously-skipped cases.
- **Filter-function CSS shorthand** — 30 additional `filter: blur()` / `brightness()` /
  `drop-shadow()` / ... resvg tests now pass.

### Breaking API Changes

v0.5 includes a substantial API refactor to prepare for future GPU rendering and to unify parser
error reporting. Callers upgrading from v0.1 will need to update the following:

- **`Transform` → `Transform2`, `Box` → `Box2`** — 2D math types are renamed to make room for
  future 3D variants. No compatibility aliases are provided; callers must update to the new
  names (and the `Transform2f`/`Transform2d`/`Box2d` shorthand typedefs).
- **`PathSpline` replaced by immutable `Path` + `PathBuilder`** — The mutable `PathSpline` class
  is removed. Paths are now constructed via `PathBuilder` and consumed as immutable `Path`
  values. `Path` exposes geometric queries (`bounds`, `transformedBounds`, `pathLength`,
  `pointAt`, `tangentAt`, `normalAt`, `strokeMiterBounds`, `isInside`, `isOnPath`, `vertices`)
  and conversions (`cubicToQuadratic`, `toMonotonic`, `flatten`, `strokeToFill`). `PathBuilder`
  supports native `QuadTo` and full SVG Appendix F.6 arc decomposition.
- **`BezierUtils`** — New public utility with `EvalQuadratic/Cubic`, `SplitQuadratic/Cubic`,
  `ApproximateCubicWithQuadratics`, `QuadraticYExtrema/CubicYExtrema`, and
  `QuadraticBounds/CubicBounds`.
- **`FillRule` moved from `donner::svg` to `donner`** — Now lives in the base namespace since
  it is shared across the path and rendering layers.
- **`PathParser` emits native `QuadTo`** — Previously quadratic segments were degree-elevated to
  cubics. Consumers that assumed only cubic segments will need to handle `QuadTo` as well.
- **Unified parser diagnostics (`ParseError` → `ParseDiagnostic`)** — All parser APIs now return
  `ParseDiagnostic` instead of `ParseError`. Diagnostics carry a `DiagnosticSeverity`
  (`Error`/`Warning`), a reason string, and a half-open `SourceRange` (replacing the previous
  single `FileOffset`). Use `ParseDiagnostic::Error()` / `ParseDiagnostic::Warning()` factories
  to construct them.
- **`ParseWarningSink`** — Parser entry points now accept a `ParseWarningSink&` for collecting
  non-fatal warnings, replacing `std::vector<ParseError>*` out-parameters. The sink uses lazy
  factory-callable evaluation, so warning construction is zero-cost when no sink is attached.
  A new `DiagnosticRenderer` produces clang/rustc-style output with source context and
  caret/tilde indicators.

**Full Changelog**: https://github.com/jwmcglynn/donner/compare/v0.1.2...v0.5.0

### What's Included

- `donner-svg`, a CLI tool for rendering and previewing SVG files.
- An embeddable C++ API for parsing, inspecting, modifying, and rendering SVGs.

```cpp
ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(fileData);
if (maybeDocument.hasError()) {
  std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
  std::abort();
}

Renderer renderer;
renderer.draw(maybeDocument.result());

const bool success = renderer.save("output.png");
```

To use the CLI tool:

```sh
# Render to PNG
donner-svg input.svg --output output.png

# Show a terminal preview
donner-svg input.svg --preview

# Interactive terminal mode with mouse selection
donner-svg input.svg --interactive
```

Or when building from source:

```sh
bazel run //donner/svg/tool -- input.svg --output output.png
```

---

## v0.1.2

This release provides an embeddable SVG2 library, which includes CSS3 with a Skia-based renderer.

Large portions of the SVG2 static spec are implemented, including all shapes, markers, gradients,
`<use>`, etc. Text is not yet implemented but is planned for `v0.2.0`.

### What's Changed

- Add support for the [`<symbol>`](https://jwmcglynn.github.io/donner/xml_symbol.html) element.
- Experimentally support XML `<!ENTITY ...>` references in the doctype (behind the off-by-default
  [`enableExperimental` Option](https://jwmcglynn.github.io/donner/structdonner_1_1svg_1_1parser_1_1SVGParser_1_1Options.html)).
- Various dependency updates.

**Full Changelog**: https://github.com/jwmcglynn/donner/compare/v0.1.1...v0.1.2

### What's Included

- `svg_to_png`, which is a standalone tool to render an SVG using Donner.
- An embeddable C++ API that allows rendering and manipulating SVGs.

---

## v0.1.1

This release provides an embeddable SVG2 library, which includes CSS3 with a Skia-based renderer.

Large portions of the SVG2 static spec are implemented, including all shapes, markers, gradients,
`<use>`, etc. Text is not yet implemented but is planned for `v0.2.0`.

### What's Changed

- Improve error handling in `svg_to_png` app.
- Fixed `SVGElement::tryCast` and `SVGElement::isa` with intermediate classes like
  `SVGGraphicsElement`.
- Always build Skia optimized, even in debug builds.
- Fixed source offsets for namespace mismatch errors in XML.
- Remove dependency on absl, replace `absl::from_chars` with a hand-rolled float parser.
- Various dependency updates.

**Full Changelog**: https://github.com/jwmcglynn/donner/compare/v0.1.0...v0.1.1

### What's Included

- `svg_to_png`, which is a standalone tool to render an SVG using Donner.
- An embeddable C++ API that allows rendering and manipulating SVGs.

---

## v0.1.0

Donner SVG's first release!

This release provides an embeddable SVG2 library, which includes CSS3 with a Skia-based renderer.

Large portions of the SVG2 static spec are implemented, including all shapes, markers, gradients, `<use>`, etc.

Included in the release are:

- `svg_to_png`, which is a standalone tool to render an SVG using Donner.
- An embeddable C++ API that allows rendering and manulating SVGs.

```cpp
ParseWarningSink warnings;
ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(fileData, warnings);
if (maybeDocument.hasError()) {
  std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
  std::abort();
}

Renderer renderer;
renderer.draw(maybeDocument.result());

const bool success = renderer.save("output.png");
```

To use svg_to_png:

```sh
./svg_to_png donner_splash.svg
```

Or when building from source:

```sh
bazel run --run_under="cd $PWD &&" //examples:svg_to_png -- donner_splash.svg
```
