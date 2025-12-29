# SVG2 color space support design

## Overview

SVG2 and CSS Color 4 (CRD 2025-04-24) standardize richer color spaces beyond sRGB. Earlier
revisions of Donner resolved most CSS color functions directly to sRGB and only recognized
`color()` arguments for `srgb` and `srgb-linear`. This document describes the shipped color model,
parser, and conversion pipeline that now cover the CSS Color 4 functions and named profiles while
keeping color management centralized in the `Color` class and downstream render backends. Inter-
and intra-space interpolation remain delegated to renderers. Deterministic sRGB conversion acts as
the default fallback.

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

## Spec coverage (CSS Color 4 CRD 2025-04-24)

Reference: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/

| Area | Status | Notes |
| --- | --- | --- |
| [rgb()][css-rgb] | Complete | Modern alpha syntax; comma or space separators. |
| [rgba()][css-rgb] | Complete | Legacy alias parsed alongside `rgb()`. |
| [hsl()][css-hsl] | Complete | Modern alpha syntax; comma or space separators. |
| [hwb()][css-hwb] | Complete | Modern alpha syntax; comma or space separators. |
| [lab()][css-lab] | Complete | Deferred conversion; channels checked on resolve. |
| [lch()][css-lab] | Complete | Deferred conversion; channels checked on resolve. |
| [oklab()][css-oklab] | Complete | Deferred conversion; channels checked on resolve. |
| [oklch()][css-oklab] | Complete | Deferred conversion; channels checked on resolve. |
| [color() profiles][css-color-fn] | Complete | Supports sRGB/P3/A98/ProPhoto/Rec2020/XYZ D50/D65. |
| [Profile aliases][css-changes] | Complete | Stylesheet aliases for profiles (Color 5 log). |
| [device-cmyk()][css-device-cmyk] | Complete | Parses CMYK with optional alpha or RGB fallback. |
| [Relative colors][css-relative] | Complete | Parses `from` syntax across supported functions. |
| [Interpolation][css-interpolation] | Partial | sRGB fallback; backends interpolate. |

[css-rgb]: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#rgb-functions
[css-hsl]: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#the-hsl-notation
[css-hwb]: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#the-hwb-notation
[css-lab]: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#cie-lab
[css-oklab]: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#ok-lab
[css-color-fn]: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#color-function
[css-changes]: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#changes-from-20240213
[css-device-cmyk]: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#device-cmyk
[css-relative]: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#relative-colors
[css-interpolation]: https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#interpolation

## Current capabilities

- `Color` retains authored color spaces through `ColorSpaceValue` and defers conversion until
  rendering via `Color::asRGBA()`.
- `color()` parsing accepts `srgb`, `srgb-linear`, `display-p3`, `a98-rgb`, `prophoto-rgb`,
  `rec2020`, `xyz-d50`, and `xyz-d65`, carrying channel values without early clamping.
- `@color-profile` rules can alias the supported profiles to custom names consumed by `color()`.
- `hsl()`, `hwb()`, `lab()`, `lch()`, `oklab()`, and `oklch()` preserve authored values and rely on
  the shared conversion helpers when resolving to sRGB.
- Relative color parsing reuses base color components across color spaces, including the `color()`
  function.
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
