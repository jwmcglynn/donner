# resvg-test-suite: Custom Golden Overrides

**Status:** Reference (current as of 2026-08-18)

When Donner's rendering of a resvg-test-suite case is correct but the upstream
golden is _not_ the right thing to compare against, the test pins
`Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-<name>.png")`
so it compares against a Donner-blessed golden instead. This doc is the catalog
of every such override and the reason it exists.

There are two distinct reasons an override is justified:

1. **resvg's golden is wrong per spec** — Donner renders to the spec, resvg
   doesn't. These are genuine upstream golden bugs.
2. **Blessed minor diff** — Donner and resvg both render acceptably, but there's
   a small, well-understood difference (sub-pixel char advance, anti-aliasing on
   a curved baseline, a font-backend difference) where pinning a Donner golden is
   cleaner than carrying a per-test pixel threshold.

Custom goldens live in `donner/svg/renderer/testdata/golden/`: `resvg-<test-name>.png`,
named with the test stem, and `donner-<test-name>.png` for the Donner renderings whose evidence
is in the [golden README](../../donner/svg/renderer/testdata/golden/README.md). The rationale for each is
the `.withReason("…")` argument chained onto the override in
[`resvg_test_suite.cc`](../../donner/svg/renderer/tests/resvg_test_suite.cc) — grep
the test name there to find it inline.

## Invalid filter URL reference qualifications

Filter Effects 1 specifies that a missing or non-filter URL disables the complete filter chain,
leaving the object unfiltered ([published definition](https://www.w3.org/TR/filter-effects-1/#FilterProperty),
[editor's draft](https://drafts.csswg.org/filter-effects-1/#FilterProperty)).
Two historical PNGs encode different behavior:

- `filters/filter/invalid-FuncIRI.svg`: the PNG makes the referenced rectangle transparent.
- `filters/filter-functions/one-invalid-url-in-list.svg`: the PNG retains the surrounding
  grayscale and opacity functions; its center is RGBA `(80,126,100,127)`.

Both files are reference-disagreement skips against those PNGs. Independently authored pixel
tests in `FilterReferenceStages_tests.cc` require unchanged visible leaf and group geometry for
missing and non-filter references, including functions on both sides of a bad URL. No
replacement golden or enlarged tolerance is used. Empty valid filters are covered separately and
remain transparent.

## Blur at a clipping filter region

`filters/filter/complex-order-and-xlink-href.svg` filters a rect through a filter that inherits
its blur and height through `xlink:href` and sets its own `x`, so the filter region cuts the
blur's input at its left and bottom edges. resvg approximates the blur with five box passes over a
region-sized buffer, and each pass drops what spreads past the buffer, so the first row and column
inside read alpha 84. Donner keeps that spill and reads 143, close to the 144.4 of the exact
Gaussian of the clipped input; resvg v0.49.0 renders like the vendored reference. The row compares
at default settings against `donner-complex-order-and-xlink-href.png`, a Donner rendering whose
evidence is in the [golden README](../../donner/svg/renderer/testdata/golden/README.md).
`RendererRegressionTests.FilterHrefRegionClipMatchesClippedSourceBlurredWithRoom` independently
requires the clipped render to equal the visible part of the rect blurred with room around it.

## Active overrides

### resvg golden is wrong per spec

| Category                   | Test                                  | Reason (`withReason`)                                          |
| -------------------------- | ------------------------------------- | -------------------------------------------------------------- |
| filters/feSpotLight        | complex-transform                     | resvg bug: SpotLight Y                                         |
| filters/feImage            | svg                                   | We render higher quality                                       |
| filters/filter             | complex-order-and-xlink-href          | Reference disagreement: resvg drops blur spill at the region edge |
| filters/feColorMatrix      | (7 identity forms, shared golden)     | Reference disagreement: the reference stores linearRGB in 8 bits |
| filters/filter-functions   | drop-shadow-function (4 rows)         | Reference disagreement: the references truncate the shadow offset |
| filters/feSpecularLighting | with-fePointLight                     | resvg golden                                                   |
| painting/paint-order       | on-tspan                              | Preserve cross-span kerning across a paint-only tspan boundary |
| painting/stroke            | linear-gradient-on-text               | Reference disagreement: paint mapped by glyph ink extents      |
| painting/stroke            | radial-gradient-on-text               | Reference disagreement: paint mapped by glyph ink extents      |
| structure/image            | embedded-svg-with-text                | Harness mismatch: SVG images get the suite's test fonts        |
| text/font-size             | named-value                           | Donner uses CSS Fonts Level 4                                  |
| text/font-size             | named-value-without-a-parent          | Donner uses CSS Fonts Level 4                                  |
| text/font-size-adjust      | simple-case                           | Reference disagreement: anchor on the shaped advance (#1173)   |
| text/textPath              | mixed-children-1                      | Reference disagreement: SVG 2 continues at the path end point  |
| text/textPath              | mixed-children-2                      | Reference disagreement: SVG 2 continues at the path end point  |
| text/textPath              | very-long-text                        | Reference disagreement: SVG 2 continues at the path end point  |

`painting/stroke/{linear,radial}-gradient-on-text` compare against independently captured
Firefox renderings at the suite's default comparison settings. The vendored images map the
gradient through glyph ink extents. SVG's text bounding box uses full glyph cells. See the
[reference evidence](../../donner/svg/renderer/testdata/golden/README.md#gradient-strokes-on-text).

`structure/image/embedded-svg-with-text` is a harness mismatch: it runs against a Donner CPU
rendering (`resvg-embedded-svg-with-text.png`), with SVG image documents given the suite's test
fonts by the harness; see [0021](0021-resvg_feature_gaps.md) and
[`testdata/golden/README.md`](../../donner/svg/renderer/testdata/golden/README.md).

The seven feColorMatrix identity forms (without-attributes, type=matrix without values or with
empty, short or long value lists, and type=saturate and type=hueRotate without a value) share
`donner-feColorMatrix-identity.png`; the evidence that the 8-bit tables posterize the references
is in the [golden README](../../donner/svg/renderer/testdata/golden/README.md).

`named-value{,-without-a-parent}` reflect CSS Fonts Level 4's revised handling of
named font sizes (`xx-small` … `xx-large`), which differs from resvg's older table.

`textPath/mixed-children-2` runs against a committed Donner render
(`resvg-mixed-children-2.png`). SVG 2 continues the text after a `textPath` from the
end point of the path; the vendored PNG continues `long` and the trailing `.` from the
end of the glyphs laid out on each path. The golden's ink matches a Chrome rendering of
the case within one device pixel. The evidence and the regeneration command are in the
[golden README](../../donner/svg/renderer/testdata/golden/README.md).

`textPath/mixed-children-1` and `textPath/very-long-text` run against committed Donner
renders. SVG 2 continues the text after a `textPath` from the end point of the path, and
CSS Text 3 keeps the textPath's trailing space on the path, so the green `Text` starts at
the end point; the vendored PNGs continue from the end of the glyphs on the path. Both
renders match Chrome's ink. The evidence and the regeneration command are in the
[golden README](../../donner/svg/renderer/testdata/golden/README.md).

`font-size-adjust/simple-case` runs against a committed Donner render
(`resvg-font-size-adjust-simple-case.png`). The vendored PNG kerns `Text` but centers
it on the unkerned advance, 4.03 device px left of the kerned center, while SVG 2
anchors a text chunk on its shaped advance. resvg v0.49.0 does not implement
`font-size-adjust`, so no upstream rendering exists. The evidence and the regeneration
command are in the [golden README](../../donner/svg/renderer/testdata/golden/README.md).

### Vertical text: resvg's own model

| Category                | Test                    | Reason                                                             |
| ----------------------- | ----------------------- | ------------------------------------------------------------------ |
| text/writing-mode       | japanese-with-tb        | Upright glyphs 5.6 px below the vmtx origin; quoted-family Chromium control agrees |
| text/writing-mode       | mixed-languages-with-tb | Upright glyphs 0.4 px below the vmtx origin; Firefox agrees        |
| text/writing-mode       | tb-and-punctuation      | Punctuation forms 14 px off; Chrome matches Donner within 0.4 px   |
| text/alignment-baseline | hanging-on-vertical     | No alignment-baseline in the PNG; Chrome matches Donner to 1.1 px  |

These references encode resvg's vertical model. For the Japanese fixture, Chromium's matching control quotes the font family.
resvg centers upright glyphs on the hhea ascent and descent instead of using the font's `vmtx`,
and it does not apply `alignment-baseline` in vertical layout. CSS Writing Modes 3 section 5.1.1
requires the font's vertical metrics; Blink and Gecko take the upright origin from `VORG`, then
from the `vmtx` top side bearing, as Donner does. Measurements are in the golden README.

### Mixed vertical underline

The full-text `text/writing-mode/mixed-languages-with-tb-and-underline` comparison uses an
independently positioned outline reference and Amiri's primary-face underline metrics. It
compares the full 500 × 500 scene at default parameters. The golden README records the layout,
browser controls, and reproduction command.

### Reference does not depict the feature

| Category      | Test           | Reason (`withReason`)                                       |
| ------------- | -------------- | ----------------------------------------------------------- |
| text/textPath | method=stretch | Reference disagreement: the vendored PNG is align placement |

`textPath/method=stretch` runs against a committed Donner render
(`resvg-method=stretch.png`). The vendored PNG is byte-identical to the original snapshot’s `simple-case.png`,
which is align placement, so it cannot assert the stretch warp. The golden is a
regression guard for the warp rather than an independent oracle;
`RendererPublicApiTest.TextPathStretchOnStraightPathsMatchesOrdinaryRotatedText`, which
requires stretch on a straight path to equal ordinary rotated text, is the independent
check. The evidence and the regeneration command are in the
[golden README](../../donner/svg/renderer/testdata/golden/README.md).

### Allowed nearest-neighbor reference differences

CSS Images permits an implementation-selected edge-preserving algorithm for
`crisp-edges`; deprecated `optimizeSpeed` maps to that value. An alternate nearest
sampling grid does not establish that the upstream reference violates the specification.

| Category | Test | Reason |
| --- | --- | --- |
| painting/image-rendering | on-feImage | Nearest sampling at destination pixel centers |

The [golden README](../../donner/svg/renderer/testdata/golden/README.md#nearest-neighbor-sampling-for-on-feimage)
records the construction and checksums for `on-feImage`. The separate `optimizeSpeed` fixture
compares against its resvg v0.49.0 rendering on TinySkia and Geode. Geode maps destination
pixel centers through the inverse transform to select the nearest source texel.

### Color-adjust filter labels

The seven `filters/filter-functions/color-adjust-functions-*` rows retain the vendored reference
outside the label bands. Both backends compare the live filtered squares against the original
pixels and the live labels against rasterized outlines from usvg. The [golden README](../../donner/svg/renderer/testdata/golden/README.md#color-adjust-filter-references)
records the reference construction and qualification.

### Backend-difference goldens (text)

These render correctly but differ from resvg because Donner's text stack (and the
simple-vs-`text-full` split) shapes/composes differently. The golden is whichever
Donner backend the test runs under.

| Category  | Test                                         | Reason                                              |
| --------- | -------------------------------------------- | --------------------------------------------------- |
| text/text | complex-graphemes-and-coordinates-list       | Simple text can't compose combining marks           |
| text/text | compound-emojis-and-coordinates-list         | Emoji bitmap scaling differs from the golden        |

### Upright Latin and crosshair coverage

| Category | Test | Reference |
| --- | --- | --- |
| text/glyph-orientation-vertical | simple-case | Font-derived letter outlines plus the original vector crosshairs and frame |

The historical crosshair coverage accounts for 731 of 733 differing pixels. Both CPU text
tiers compare the complete scene against an outlined control with default parameters. The
golden README records the font metrics, browser controls, and regeneration command.

### Blessed minor diffs (textPath / text-decoration)

The `text/textPath` family carries the bulk of the overrides: sub-pixel character
advance and anti-aliasing differences along curved baselines, which differ between
the simple and `text-full` backends. All are `.withReason("Minor char")` unless
noted.

| Category             | Test                                     | Reason                                            |
| -------------------- | ---------------------------------------- | ------------------------------------------------- |
| text/textPath        | dy-with-tiny-coordinates                 | resvg 0.47.0 reference (golden README)            |
| text/textPath        | tspan-with-absolute-position             | Minor char                                        |
| text/textPath        | tspan-with-relative-position             | Minor char                                        |
| text/textPath        | with-underline                           | Minor char                                        |
| text/text-decoration | indirect                                 | (no reason string)                                |

> `text-decoration/indirect` carries no `withReason`. It should either get one or migrate to a
> per-test threshold. Tracked in [0021](0021-resvg_feature_gaps.md).

## Golden files

Every `resvg-*.png` and `donner-*.png` in `golden/` is referenced by an override above. The
`text/font-size` tests other than `named-value{,-without-a-parent}` compare against the upstream
references.

## Maintenance

- Adding an override: place `resvg-<new-stem>.png` in
  `donner/svg/renderer/testdata/golden/`, add `Params::WithGoldenOverride(…) .withReason("…")` in `resvg_test_suite.cc`, and add a row here. The
  `golden/*.png` glob in
  [`testdata/BUILD.bazel`](../../donner/svg/renderer/testdata/BUILD.bazel) picks it
  up automatically.
- Never overwrite a golden without explicit approval; these images encode deliberate
  spec and quality judgments.
- When a `Skip`'d gap in [0021](0021-resvg_feature_gaps.md) is fixed, check whether
  it needs an override here too.
