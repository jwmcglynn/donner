# Donner

## v0.8: Donner SVG Editor & Engine

_Date: <unreleased>_

These notes describe the upcoming v0.8 release, which has not been published yet.

v0.8 makes Donner both an SVG editor and an engine you can embed. The editor runs on the desktop
and in the browser on Donner's own GPU renderer, and the engine adds declarative animation, a
native GPU backend, and hardened handling of untrusted documents, while the compact tiny-skia CPU
renderer stays the default for embedders.

### Highlights

- **Donner SVG Editor**: a first working editor, native on macOS and Linux and in the browser.
  Select and transform shapes, draw paths with the Pen tool, edit text, and organize Layers, with
  an XML source view that stays in sync with the canvas.
- **Geode GPU renderer**: Geode renders through Donner's own GPU runtime, on Metal on macOS,
  Vulkan on Linux, and WebGPU in the browser, and drives the editor canvas. Its WGSL shaders are
  compiled while Donner builds, and shipped products need no Rust toolchain or Rust-built GPU
  library. A hung GPU is reported as a lost device instead of a hung process. Enable it with
  `--config=geode`; see the [GPU Runtime Reference](docs/gpu_runtime.md).
- **tiny-skia by default**: the compact CPU renderer remains the default backend for Bazel,
  Bazel Central Registry and CMake consumers, with no GPU dependency.
- **Declarative animation**: `<animate>`, `<set>`, `<animateTransform>`, `<animateMotion>` and
  `<mpath>` parse by default, with clock, event, syncbase and repeat timing and linear, discrete,
  paced and spline interpolation. The host supplies the clock with `SVGDocument::setTime()` and
  delivers pointer events; the editor's Preview mode plays animations, and
  `donner-svg --gif` exports animated GIFs. See
  "Animating an SVG" in [Getting Started](docs/getting_started.md).
- **Text**: create and edit text in the editor, with Bold, Italic and Underline, and convert it to
  path outlines. A frame now draws up to 65,536 glyphs by default, configurable with
  `RendererInterface::setMaximumGlyphs`.
- **Security hardening**: bounded parsing, decoding and rendering of untrusted SVG, CSS, images
  and fonts, continuous fuzzing that now includes rasterizing documents with the default renderer,
  and fixes for two heap buffer overflows in tiny-skia filter region clipping. Secure processing
  modes keep SVG images static and offline. The [threat model](docs/threat-model.md) documents the
  trust boundaries.

### Behavior Changes

- **User-defined attributes are kept by default.** `SVGParser::Options::disableUserAttributes`
  now defaults to `false`, so `data-*` and other custom attributes survive parsing and match CSS
  attribute selectors. Set it to `true` to skip them.
- **Foreign-namespace elements are kept in the tree and never render.** They count against the
  parser's tree size and depth limits.
- **Filter graphs built in code** resolve an unset second input of `feComposite`, `feBlend` and
  `feDisplacementMap` to the previous primitive's result. Parsed documents are unaffected.
- **`Path::strokeToFill`** returns overlapping positive-winding pieces that must be filled with
  `FillRule::NonZero`, and returns an empty path when the outline cannot be represented. Path
  producers can request that result with the new `PathBuilder::rejectRemainingCommands`.
- **CSS filter functions**: a negative amount in `grayscale()`, `invert()`, `opacity()` or
  `sepia()` rejects the whole `filter` declaration instead of clamping to zero. In presentation
  attributes, `blur()` accepts a unitless length, as in `filter="blur(4)"`.
- **`DONNER_GPU_BACKEND`** accepts only `metal` or `vulkan`, and an unset or empty value selects
  the platform's backend. Any other value, including the former `wgpu`, or a backend the process
  cannot open, stops the process with an error.

### Removed

- The full Skia renderer backend; use tiny-skia or Geode.
- The transitional WebGPU adapter and its Rust-built wgpu-native libraries from every production
  build.
- `RendererTextMaterializationBudget::kMaximumUniqueOutlines`,
  `RendererTextMaterializationBudget::kMaximumGlyphOccurrences` and
  `GeodeGlyphCache::kDefaultMaxEntries`, replaced by `kDefaultMaximumGlyphs` and
  `RendererInterface::setMaximumGlyphs`.

### Security support

v0.8 is the only version supported with security updates; v0.5 and v0.1 are no longer supported.
See `SECURITY.md`.

### What's Included

- The Donner SVG Editor as a native desktop application and a WebAssembly edition.
- `donner-svg`, a CLI tool that renders SVG files to PNG or animated GIF and previews them in the
  terminal.
- An embeddable C++20 API for parsing, inspecting, modifying, animating and rendering SVGs.
- Prebuilt `donner-svg_linux_x86_64` and `donner-svg_darwin_arm64` binaries, each with a checksum,
  build provenance and a `.NOTICE.txt` listing the third-party licenses it includes, attached by
  release automation.

Parse an SVG, find an element by its `data-*` attribute, restyle it, and render it to a PNG with
the default renderer:

<!-- example: examples/svg_quickstart.cc [quickstart] -->

```cpp
constexpr std::string_view kSvg = R"(
  <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
    <rect data-role="status" width="64" height="64" fill="gray" />
  </svg>)";

donner::ParseWarningSink warnings;
donner::ParseResult<donner::svg::SVGDocument> maybeDocument =
    donner::svg::parser::SVGParser::ParseSVG(kSvg, warnings);
if (maybeDocument.hasError()) {
  std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
  return 1;
}

donner::svg::SVGDocument document = std::move(maybeDocument.result());

// Custom data-* attributes are kept by default, so attribute selectors match them.
std::optional<donner::svg::SVGElement> status = document.querySelector("[data-role=status]");
if (!status) {
  std::cerr << "No element has data-role=status\n";
  return 1;
}
status->setStyle("fill: green");

donner::svg::Renderer renderer;
renderer.draw(document);
if (!renderer.save("output.png")) {
  std::cerr << "Could not save output.png\n";
  return 1;
}
```

The complete program is `examples/svg_quickstart.cc`; run it with
`bazel run //examples:svg_quickstart`.

**Full Changelog:** generated when the `v0.8.0` tag is cut.

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
