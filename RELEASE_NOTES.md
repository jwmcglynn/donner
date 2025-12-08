# Donner

## Unreleased

- Added full SVG2 color-space coverage for CSS color functions, including `color()` profiles,
  OKLab/OKLCH, Lab/LCH, HSL, and HWB with deferred conversion through `Color::asRGBA()`.
- Implemented `@color-profile` parsing with alias binding to the supported SVG2 profiles.
- Documented the color-space architecture and validation approach for future HDR/ICC work.
- Tracked in issue #6 to confirm all CSS Color 4 functions are implemented and tested.

## v0.1.0

Donner SVG's first release!

This release provides an embeddable SVG2 library, which includes CSS3 with a Skia-based renderer.

Large portions of the SVG2 static spec are implemented, including all shapes, markers, gradients,
`<use>`, etc. Text is not yet implemented but is planned for `v0.2.0`.

Included in the release are:

- `svg_to_png`, which is a standalone tool to render an SVG using Donner.
- An embeddable C++ API that allows rendering and manipulating SVGs.

```cpp
ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(fileData);
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
