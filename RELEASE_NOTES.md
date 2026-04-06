# Donner

## v0.5.0

Major release adding a software rendering backend, complete text rendering, all 17 SVG filter
primitives, and a CLI tool — while maintaining the existing Skia backend.

### Highlights

- **tiny-skia software renderer** — A new default rendering backend written in C++ that requires no
  external GPU libraries. Achieves performance within 1.5x of Skia across all rendering operations.
  The Skia backend remains available as a build option.
- **Text rendering** — Full `<text>`, `<tspan>`, and `<textPath>` support with per-character
  positioning, text-anchor, text-decoration, and all baseline properties. Fonts are loaded from
  `@font-face` rules (TTF, OTF, WOFF, WOFF2). An optional HarfBuzz tier (`--config=text-full`)
  adds complex script shaping.
- **All 17 SVG filter primitives** — feGaussianBlur, feColorMatrix, feComposite, feBlend,
  feTurbulence, feConvolveMatrix, feDiffuseLighting, feSpecularLighting, feDisplacementMap,
  feComponentTransfer, feMorphology, feFlood, feOffset, feMerge, feTile, feImage, feDropShadow.
  Float-precision pipeline with SIMD (NEON) optimizations. CSS shorthand filter functions
  (`blur()`, `brightness()`, `drop-shadow()`, etc.) also supported.
- **`donner-svg` CLI tool** — Render SVGs to PNG, preview in the terminal, or interactively
  inspect elements with mouse selection.

### What's Changed

- **Renderer architecture** — Abstract `RendererInterface` with `RendererDriver` traversal,
  enabling backend-agnostic rendering. Both Skia and tiny-skia implement the same interface.
- **Incremental invalidation** — Dirty flags on DOM mutations with O(1) fast path for unchanged
  documents. `StyleSystem` performs selective per-entity recomputation.
- **External SVG references** — `<image>` and `<use>` elements can reference external `.svg` files
  with sandboxed resource loading.
- **`<clipPath>` `<use>` support** — `<use>` children inside `<clipPath>` now resolve correctly.
- **XML entity support** — Custom `<!ENTITY>` declarations in doctypes are now parsed by default.
- **XML parser conformance** — `Name` token validation per XML 1.0 spec.
- **CMake support** — Full CMake build for both Skia and tiny-skia backends with feature toggles
  for text, WOFF2, and filters.
- **Fuzzing** — Continuous fuzzing harness with Docker support; all parser surfaces fuzz-tested.
- **74%+ code coverage** across production code (80%+ excluding vendored filter library).

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
donner-svg render input.svg --output output.png
donner-svg preview input.svg
donner-svg interactive input.svg
```

Or when building from source:

```sh
bazel run //donner/svg/tool:donner-svg -- input.svg --output output.png
```

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

RendererSkia renderer;
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
