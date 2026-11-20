# resvg-test-suite: Custom Golden Overrides

**Status:** Reference. Catalogs resvg-test-suite cases that compare against a custom golden
instead of their vendored PNG, the reason for each override, and the rules an override must
meet. The [golden README](../../donner/svg/renderer/testdata/golden/README.md) records each
golden's provenance and SHA-256.

When Donner's rendering of a resvg-test-suite case is correct but the upstream
golden is _not_ the right thing to compare against, the test pins
`Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-<name>.png")`
so it compares against a Donner-blessed golden instead. This doc describes the main override
groups and their reasons; the registrations in
`donner/svg/renderer/tests/resvg_test_suite.cc` and the golden README's "Registered golden
digests" table are the complete lists.

An override is justified when the vendored reference is not the right thing to compare against:

1. **Reference disagreement**: Donner renders to the specification and the vendored PNG does
   not. These are genuine upstream golden bugs.
2. **Harness mismatch or API-policy difference**: the vendored PNG was produced under different
   harness conditions (for example an opaque background) or a different API policy.

Every override records what the vendored PNG gets wrong in its reason, has an entry in
`tools/resvg_parity/exception_policies.json`, and has a row with its SHA-256 in the golden
README's digest table. A difference whose root cause is in Donner is fixed in the engine, not
pinned with a golden, and a pixel budget in the hundreds is not accepted as a substitute. Some
rows below are regression goldens whose provenance is not recorded; their reasons say so.

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

Both rows compare at default settings against Donner goldens that draw the element unfiltered
(see the [golden README](../../donner/svg/renderer/testdata/golden/README.md)). Independently
authored pixel tests in `FilterReferenceStages_tests.cc` require unchanged visible leaf and group
geometry for missing and non-filter references, including functions on both sides of a bad URL.
No enlarged tolerance is used. Empty valid filters are covered separately and remain transparent.

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
| filters/feSpotLight        | complex-transform                     | Reference shades before skew and attenuates the cone edge differently |
| filters/filter             | complex-order-and-xlink-href          | Reference disagreement: resvg drops blur spill at the region edge |
| filters/filter             | subregion-and-primitiveUnits=objectBoundingBox-1/-2 | Explicit half-box clip; opaque interior and exact crosshair ([reference evidence](../../donner/svg/renderer/testdata/golden/README.md#gaussian-blur-primitive-subregions)) |
| filters/feColorMatrix      | (7 identity forms, shared golden)     | Reference disagreement: the reference stores linearRGB in 8 bits |
| filters/filter-functions   | drop-shadow-function (4 rows)         | Reference disagreement: the references truncate the shadow offset |
| painting/stroke            | linear-gradient-on-text               | Reference disagreement: paint mapped by glyph ink extents      |
| painting/stroke            | radial-gradient-on-text               | Reference disagreement: paint mapped by glyph ink extents      |
| structure/image            | embedded-svg-with-text                | Harness mismatch: SVG images get the suite's test fonts        |
| text/text-decoration | all-types-inline | Literal bands preserve multiline decoration offsets and paint order |
| text/font-size-adjust      | simple-case                           | Reference disagreement: anchor on the shaped advance (#1173)   |
| text/textPath              | mixed-children-1                      | Reference disagreement: SVG 2 continues at the path end point  |
| text/textPath              | mixed-children-2                      | Reference disagreement: SVG 2 continues at the path end point  |
| text/textPath              | very-long-text                        | Reference disagreement: SVG 2 continues at the path end point  |
| text/tspan                 | with-filter                           | Reference disagreement: the reference draws glyph edges 0.25 to 0.3 px outside the outline |
| text/textPath              | with-filter                           | Reference disagreement: the reference draws glyph edges 0.25 to 0.3 px outside the outline |

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

### Vertical text: font vertical metrics

| Category                | Test                    | Reason                                                             |
| ----------------------- | ----------------------- | ------------------------------------------------------------------ |
| text/writing-mode       | japanese-with-tb        | Upright glyphs 5.6 px below the vmtx origin                        |
| text/writing-mode       | mixed-languages-with-tb | Upright glyphs 0.4 px below the vmtx origin                        |
| text/writing-mode       | tb-and-punctuation      | No `vert` forms for the comma and full stop, 14 px off             |
| text/alignment-baseline | hanging-on-vertical     | Upright glyphs advance 1.39 em instead of the 1 em vmtx advance    |
| text/textPath           | writing-mode=tb         | Upright glyphs off the vmtx origin on a curved path                |

CSS Writing Modes 3 section 5.1.1 requires upright glyphs to use the font's vertical metrics and
vertical features (the OpenType `vert` feature must be enabled), and Mplus 1p has `vhea` and
`vmtx`. These references do not follow them, and their offsets are not one consistent model: the
Japanese glyphs sit about 35 units below the vmtx origin, where an origin centered on the hhea
ascent and descent would put them only 17.5 units below, and the hanging row places the cross-axis
baseline like Donner but advances each upright glyph by about 1.39 em. Independent contract tests
check the vmtx origins and advances. Measurements are in the golden README.

### Vertical text the specification leaves to the user agent

`text/writing-mode/mixed-languages-with-tb-and-underline` and
`text/glyph-orientation-vertical/simple-case` render without a comparison
(`Params::UndefinedBehavior`, listed in `SPEC_UB_ALLOWLIST`). CSS Text Decoration 3 makes the
position and thickness of an `auto` underline user-agent defined, and the underline is most of the
first row's difference from any reference. In the second, SVG 2 aliases
`glyph-orientation-vertical="0"` to upright text, but Noto Sans has no vertical metrics and CSS
Writing Modes 3 does not define how a user agent synthesizes them.
`RendererRegressionTests.GlyphOrientationVerticalZeroDrawsUprightGlyphs` keeps the alias itself
covered: it requires the alias to draw exactly like `text-orientation: upright` and unlike the
default sideways Latin.

### Opaque vendored references

These vendored PNGs are opaque white, while Donner's canvas and the other references are
transparent, and resvg v0.49.0 has no usable replacement. Chromium 151 draws all three rows within 47 pixels of the goldens,
and Firefox 153 draws the two `path` rows within 38 pixels but anchors the `y`-only chunk of
coordinates-list differently. `testdata/golden/README.md` has the evidence.

| Category | Test | Reason |
| --- | --- | --- |
| text/text-anchor | coordinates-list | Harness mismatch: opaque background; glyphs match under 4x4 coverage |
| text/textPath | with-path | Reference disagreement: v0.49.0 ignores `path`; opaque PNG |
| text/textPath | with-path-and-xlink-href | Reference disagreement: v0.49.0 ignores `path`; opaque PNG |

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

### Sharp pattern-tile seams (reference rasterization, not a spec violation)

The specification does not say how a pattern tile is rasterized, so these are not "wrong per
spec". Donner does not draw pattern tiles analytically: both backends rasterize each tile at
twice its device scale and sample it bilinearly. On the two self-recursive rows each tile-edge
stroke edge falls on a half raster pixel, so that sampling reproduces the exact single-layer seam
coverage (64, 255, 255, 64 on the pixel-aligned vertical seam). `recursive-on-child` draws two
coincident half-stroke layers composited in the tile raster: its vertical seam reads 96 where
either layer alone reads 64, and neither value is the true coverage of the two strokes (see the
golden README). Content elsewhere in a tile is not reproduced exactly. The vendored PNGs resample
a rasterized tile with a softer profile. Keeping the sharp seams is a maintainer decision.
`testdata/golden/README.md` has the measurements, including what current Chromium and Firefox
draw: exact coverage on pixel-aligned seams and a resampled tile on fractional ones.

| Category | Test | Reason |
| --- | --- | --- |
| paint-servers/pattern | recursive-on-child | Sharp two-layer tile-edge stroke (seam 96, see the golden README), recursive fill one level deep in both rects; the PNG resamples a raster tile and draws the second layer in rect4 only |
| paint-servers/pattern | out-of-order-referencing | resvg 0.49.0 render of explicit tile rectangles; against the authored nested tile geometry, 832 pixels are opaque in one PNG and empty in the other |
| paint-servers/pattern | text-child | resvg 0.49.0 render of explicit clipped text cells under skew; the PNG resamples a raster tile, spreading edges into empty cells |
| paint-servers/pattern | self-recursive-on-child | Sharp tile-edge stroke at this alignment; the PNG resamples a raster tile |
| paint-servers/pattern | self-recursive | Sharp tile-edge stroke at this alignment; the PNG resamples a raster tile |

### Color-adjust filter labels

The seven `filters/filter-functions/color-adjust-functions-*` rows retain the vendored reference
outside the label bands. Both backends compare the live filtered squares against the original
pixels and the live labels against rasterized outlines from usvg. The [golden README](../../donner/svg/renderer/testdata/golden/README.md#color-adjust-filter-references)
records the reference construction and qualification.

### Named font sizes (CSS Fonts 4 table, a maintainer decision)

The absolute-size table in CSS Fonts 4 section 2.5 is a guideline for user agents, not a
requirement, so these references are not wrong per spec. Donner follows that table (3/5, 3/4,
8/9, 1, 6/5, 3/2 and 2 times `medium`) by maintainer decision; the vendored PNGs step by 1.2 per
keyword, as CSS 2 did. `testdata/golden/README.md` has the measured sizes, including the table
current browsers use.

| Category | Test | Reason |
| --- | --- | --- |
| text/font-size | named-value | Reference disagreement: CSS Fonts 4 table versus 1.2 steps |
| text/font-size | named-value-without-a-parent | Reference disagreement: CSS Fonts 4 table versus 1.2 steps |

### Backend-difference goldens (text)

These cases require full CPU text. Their references cover combining-mark and
emoji cluster placement.

| Category  | Test                                         | Reference                                           |
| --------- | -------------------------------------------- | --------------------------------------------------- |
| text/text | complex-graphemes-and-coordinates-list       | Independent resvg v0.49.0 render of the equivalent precomposed glyph    |
| text/text | compound-emojis-and-coordinates-list         | resvg v0.47.0 rendering of explicitly placed emoji clusters |

The separate `text/text/compound-emojis` and `text/text/emojis` fixtures use
resvg v0.49.0 references. All three emoji rows compare at a 0.07 threshold: the
glyphs agree at the default, and the references draw the vertical crosshair guide
1/8 px right of its exact coverage. The
[golden README](../../donner/svg/renderer/testdata/golden/README.md#color-emoji-bitmap-sampling)
records the comparison.

### textPath overrides

| Category             | Test                         | Reason |
| -------------------- | ---------------------------- | ------ |
| text/textPath        | dy-with-tiny-coordinates     | Reference disagreement: the vendored PNG predates resvg's current dy layout on a path; independent resvg 0.47.0 reference |
| text/textPath        | tspan-with-absolute-position | Regression golden for absolute distance along the text path (legacy PNG, provenance not recorded) |
| text/textPath        | with-underline               | Regression golden for the on-path underline (legacy PNG, provenance not recorded) |

The golden README records the evidence and, where known, the regeneration command for each.

## Golden files

The golden README's "Registered golden digests" table lists every golden a resvg suite row
compares against. `resvg-tspan-with-relative-position.png` is the one `resvg-*.png` or
`donner-*.png` in `golden/` that no row uses; `TextPathGeode_tests.cc` compares Geode's text-on-path rendering with it. The
`text/font-size` tests other than `named-value{,-without-a-parent}` compare against the upstream
references.

## Maintenance

- Adding an override: place `resvg-<new-stem>.png` in
  `donner/svg/renderer/testdata/golden/`, add `Params::WithGoldenOverride(…) .withReason("…")` in `resvg_test_suite.cc`,
  add the golden's SHA-256 row to the golden README's digest table, and add a row here. The
  `golden/*.png` glob in
  [`testdata/BUILD.bazel`](../../donner/svg/renderer/testdata/BUILD.bazel) picks it
  up automatically.
- Never overwrite a golden without explicit approval; these images encode deliberate
  spec and quality judgments.
- When a `Skip`'d gap in [0021](0021-resvg_feature_gaps.md) is fixed, check whether
  it needs an override here too.
