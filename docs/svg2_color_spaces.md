# SVG2 color space support design

## Overview

SVG2 and CSS Color 4 standardize richer color spaces beyond sRGB. Earlier revisions of Donner
resolved most CSS color functions directly to sRGB and only recognized `color()` arguments for
`srgb` and `srgb-linear`. This document outlines how we extended the color model, parser, and
conversion pipeline to cover additional SVG2 color spaces while keeping color management
centralized in the `Color` class and downstream render backends. The implementation is complete
end-to-end; future work is limited to optional ICC profile loading and HDR tone-mapping research.

## Tracking

- Issue: https://github.com/jwmcglynn/donner/issues/6
- Scope: Complete CSS Color 4 function coverage (`hsl()`, `hwb()`, `lab()`, `lch()`, `oklab()`,
  `oklch()`, and `color()`), deferred color conversion, and `@color-profile` parsing.

## Goals

- Represent authored color-space intent instead of eagerly flattening to sRGB in the parser.
- Parse and store all SVG2/CSS Color 4 color spaces used by `color()` and `@color-profile`.
- Provide deterministic conversion to RGBA for render backends that only support sRGB.
- Keep conversion math reusable for future HDR/backing store enhancements.

## Non-goals

- Implement full ICC profile loading beyond named SVG2 profiles (`@color-profile` only binds to
  predefined profiles in this phase).
- Add HDR tone mapping or gamut-clipping heuristics beyond simple clipping.
- Modify rendering backends; backends may still receive sRGB after conversion.

## Current capabilities

- `Color` retains authored color spaces through `ColorSpaceValue` and defers conversion until
  rendering via `Color::asRGBA()`.
- `color()` parsing accepts `srgb`, `srgb-linear`, `display-p3`, `a98-rgb`, `prophoto-rgb`,
  `rec2020`, `xyz-d50`, and `xyz-d65`, carrying channel values without early clamping.
- `@color-profile` rules can alias the supported profiles to custom names consumed by `color()`.
- `hsl()`, `hwb()`, `lab()`, `lch()`, `oklab()`, and `oklch()` preserve authored values and rely on
  the shared conversion helpers when resolving to sRGB.
- Conversion helpers encode/decode channels, perform RGB↔XYZ transforms, and apply Bradford
  adaptation when necessary.

## Architecture overview

### Color-space metadata

Profile data lives in `donner/css/ColorSpaces.h` and exposes primaries, white points, and transfer
functions as constexpr records keyed by `ColorSpaceId`. The working white point is D65; Bradford
adaptation is applied automatically when converting from D50 sources such as `xyz-d50`.

### Parser and model

The CSS parser emits structured `ColorSpaceValue` nodes for CSS color functions and `@color-profile`
definitions. Channel values remain unclamped until conversion. `Color` stores these variants
straight through so downstream systems can either resolve to sRGB or hand the authored profile to
future HDR-aware backends.

### Conversion pipeline

`Color::asRGBA()` dispatches on the stored variant and routes through helpers for encoding/decoding
transfer functions and applying RGB↔XYZ matrix multiplication. HSL/OKLab/OKLCH conversions normalize
to linear sRGB before using the shared helpers to ensure consistent alpha handling and clamping.

### Profile aliasing

`ColorProfileRegistry` maps `@color-profile` aliases to supported profiles. Lookup happens during
`color()` parsing so authored documents can bind profile names to canonical IDs without embedding
full ICC data.

## Validation

Unit tests cover parser behavior for all supported profiles, the `@color-profile` binding flow, and
the color-function conversions through `Color::asRGBA()`. The test matrix explicitly exercises the
CSS Color 4 functions that prompted [issue #6](https://github.com/jwmcglynn/donner/issues/6),
verifying `hsl()`, `hwb()`, `lab()`, `lch()`, `oklab()`, `oklch()`, and `color()` against spec
reference values. Parser tests run under Bazel via `//donner/css/parser:parser_tests`.

## Future work

Potential extensions include full ICC profile loading, HDR tone mapping, and more advanced gamut
management strategies. The current implementation deliberately clips out-of-range channels to keep
rendering predictable on sRGB backends.
