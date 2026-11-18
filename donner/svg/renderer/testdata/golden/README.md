# Text path reference images

The two references below are generated independently with resvg 0.47.0, using the
unchanged vendored SVG inputs and only the corpus `NotoSans-Regular.ttf` font.
They replace references that predate the current relative-position layout. Donner
output is never used to generate these references.

Font SHA-256: `6b04c8dd65af6b73eb4279472ed1580b29102d6496a377340e80a40cdb3b22c9`.

| Case                           | SVG SHA-256                                                        | PNG SHA-256                                                        |
| ------------------------------ | ------------------------------------------------------------------ | ------------------------------------------------------------------ |
| `tspan-with-relative-position` | `c751ba99bdc9e453507029cc0b3333b44c161542e821913009813525061c2670` | `fda4f9409e5b1cd52b3368157eca7ff5af20b438c68eb82b0e263b0cb33b8e83` |
| `dy-with-tiny-coordinates`     | `396e4edd7ad7bde78ab0b64b1b06328853b78860065be0d3aac58096b4a7f4a0` | `518e1ba79b2d93a906aecf1d109fdb3362794b36fc2b7ac4df28cc4b80fa26a4` |

Run these commands from the repository root:

```sh
resvg --skip-system-fonts --use-font-file third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf --width 500 third_party/resvg-test-suite/tests/text/textPath/tspan-with-relative-position.svg donner/svg/renderer/testdata/golden/resvg-tspan-with-relative-position.png
resvg --skip-system-fonts --use-font-file third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf --width 500 third_party/resvg-test-suite/tests/text/textPath/dy-with-tiny-coordinates.svg donner/svg/renderer/testdata/golden/resvg-dy-with-tiny-coordinates.png
```

Both compare at the ordinary 100-pixel budget. `dy-with-tiny-coordinates` sets a 0.24 font size
under `scale(100)`; its former 1100-pixel allowance covered full-tier advances rounded to 1/64
pixel of that tiny face, which the full tier no longer does.

## Emoji positioning reference

`resvg-compound-emojis-and-coordinates-list.png` is generated independently with
resvg 0.47.0 and the corpus `NotoColorEmoji.ttf`, with system fonts disabled.
Donner output is not used to generate this reference. The unchanged corpus SVG
uses `y="90 111 130 150"`. [SVG2 positioning](https://www.w3.org/TR/SVG2/text.html#TextLayoutAlgorithm)
indexes that list by codepoint, while positioning complete typographic characters:
the smile starts at index 0, the crab at index 1, and the joined rainbow flag at
index 2. Their baselines are therefore 90, 111, and 130. The internal variation
selector's value is skipped. The old image placed the crab and flag at 130 and
150, consistent with counting supplementary characters as two UTF-16 units.

To reproduce, copy the corpus SVG to `reference.svg` and replace only its `text`
element with this equivalent fragment. Keeping each complete cluster in its own
span avoids splitting the flag on an internal absolute position. Font, font size,
viewBox, crosshair, frame, and implicit horizontal advances stay the same:

```xml
<text id="text1" x="52" y="90"><tspan>&#x1F601;</tspan><tspan y="111">&#x1F980;</tspan><tspan y="130">&#x1F3F3;&#xFE0F;&#x200D;&#x1F308;</tspan></text>
```

```sh
resvg --skip-system-fonts --use-font-file third_party/resvg-test-suite/fonts/NotoColorEmoji.ttf --width 500 --height 500 reference.svg donner/svg/renderer/testdata/golden/resvg-compound-emojis-and-coordinates-list.png
```

The existing per-pixel threshold remains 0.1; the comparison uses the ordinary
100-pixel budget, replacing the former 1100-pixel allowance.

Font SHA-256: `21fc478e003fc871200ba3bcf4215295a80e06a5c1b96f5368adc2183956f01a`.

Corpus SVG SHA-256: `e65aef72d28ca80b2ad7c3830da8e06a7a50eb685c751525e0450f3ea9c4dcdc`.

Reference PNG SHA-256: `9fef8a7afaf51ff396e4a776da3ec65f72f1b3a5c3ed59e55bd4b10d5ca2061f`.

This reference qualifies the full-text CPU rendering path. Geode currently skips
bitmap-only fonts in its text renderer; that GPU capability remains unsupported.
The native positioning regression uses an independently authored joined outline
cluster, while Full TextEngine tests retain bitmap-emoji addressing and carry.

## Donner-rendered font-size-adjust reference

`resvg-font-size-adjust-simple-case.png` is Donner's own rendering of the unchanged
corpus `text/font-size-adjust/simple-case.svg`. It is used because no valid
upstream rendering exists: the vendored PNG kerns `Text` but centers it on the
unkerned advance, and resvg v0.49.0 does not implement `font-size-adjust` (it draws
the text at 64px).

Evidence, measured as ink centroids of the two vertical stems in a band below the
T crossbar, in device px at the 500x500 (2.5x) canvas:

| Stem | Vendored PNG | This reference | Offset |
| ---- | ------------ | -------------- | ------ |
| T    | 184.89       | 188.92         | 4.03   |
| t    | 311.83       | 315.86         | 4.03   |

The stem-to-stem distance is equal within 0.01 px, so both images draw the same
adjusted size (64 * 0.3 / 0.536 = 35.82, from Noto Sans's OS/2 x-height of 536
units) and the same kerned advances. Noto Sans shapes `Text` to 2010 units unkerned
and 1920 kerned (T-e -70, e-x -20). Half of the 90 units of kerning is
45 * 35.82 / 1000 = 1.61 user units = 4.03 device px, the whole offset.
[SVG 2 text-anchor](https://www.w3.org/TR/SVG2/text.html#TextAnchoringProperties)
centers the chunk on its shaped advance. `text/text-anchor/middle-on-text`, the
same kerned string centered without `font-size-adjust`, matches its upstream
reference at default settings, and
`RendererRegressionTests.FontSizeAdjustMatchesExplicitUsedSize` requires the
adjusted rendering to equal the text drawn at the explicit used size.

The image was created from the simple text tier on TinySkia with the suite's
golden-creation path, with the renderer sources of main at
`21c59c7588b1f64ecd4df30496e975544d194c61`. The suite compares it at default settings on both CPU text
tiers; the Geode resvg lane skips `text/*` rows through its backend feature gate.

Corpus SVG SHA-256: `db28cd3a01650a98afe5d8f7a718434c1e71802b68b8b7a3d8d2feb8f97f11ea`.

Reference PNG SHA-256: `7493132a5ca79910e11d3ece3c025cab71a14360162feb6e1e550df1c2762abb`.

To regenerate after an intended rendering change, delete the image, run the
command below from the repository root, and review the new image before
committing it:

```sh
rm donner/svg/renderer/testdata/golden/resvg-font-size-adjust-simple-case.png
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run //donner/svg/renderer/tests:resvg_test_suite_impl -- '--gtest_filter=*TextFontSizeAdjust*'
```

## structure/image: embedded SVG with text

Disposition: harness mismatch. `resvg-embedded-svg-with-text.png` is a Donner CPU rendering of the
unchanged corpus file `tests/structure/image/embedded-svg-with-text.svg`, at the 500x500 size of its
vendored reference, rendered by the resvg suite harness (which gives SVG image documents the corpus
fonts as a system font set) at the commit that adds it. Both CPU text tiers match it exactly; Geode
differs by 2 pixels, within the default settings.

The upstream references are not used. The vendored reference omits the embedded document's text,
which browsers and resvg draw. The resvg v0.49.0 rendering (300x300) draws it, but differs from
Donner by 469 pixels on the CPU backend and 941 on Geode. At that size the embedded document's
frame lines are 1.2 px wide, with an exact coverage of 0.8 and 0.4. Geode produces that on all
four sides and the CPU backend on the vertical sides; the reference has 0.75 and 0.5, the quarter
steps of 4x4 supersampled coverage. Its glyph edges differ only below the pixelmatch
anti-aliasing filter.

Corpus SVG SHA-256: `f469e4ae7b284f434486f6781db4a81f2ee4c86e8f3948969b53769fd26b8aee`.

PNG SHA-256: `0f0c328a6c2c99e3ff09b3e4e0c8173d0720737fcafee7c4dbfb8f316cff0bf8`.

To regenerate, delete the PNG and run the row with `UPDATE_GOLDEN_IMAGES_DIR` set; the harness
writes a missing golden from the current rendering:

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run //donner/svg/renderer/tests:resvg_test_suite_default_text -- --gtest_filter='*embedded_svg_with_text*'
```

## Donner renderings of filter references that resvg gets wrong

The goldens below are Donner renderings for rows where the vendored resvg reference does not follow
the specification. A self-rendered golden cannot prove itself correct, so each row's section gives
independent evidence, how resvg v0.49.0 renders the row, and what the suite's results.csv records:
those are manual visual grades of the vendored reference by browser and renderer, which show
whether an engine draws the case's intended picture but cannot resolve differences of a level or
a pixel along an edge.

### `filters/filter/complex-order-and-xlink-href`

Disposition: reference disagreement. `donner-complex-order-and-xlink-href.png` is a Donner CPU
rendering of the unchanged corpus file, at the 300x300 size of the resvg v0.49.0 reference,
rendered by the resvg suite harness at the commit that adds it. The software backend compares with
it at default settings; Geode differs by 3 pixels.

The filter region's x and height cut the blur's input. Filter Effects 1 makes the filter region a
hard clip on a primitive's input and result, and allows three successive box blurs only as an
approximation of the Gaussian convolution of that input. resvg blurs with five 8-bit box passes
inside a buffer the size of the region, so each pass drops what the previous one spread past the
region. Modeling those five passes on the clipped step gives 84 for the first row inside the edge,
which is the reference's value; the same passes keeping their spill give 143, Donner's value, and
the exact Gaussian of the clipped input gives 144.4 (standard deviation 3 device pixels). resvg
v0.49.0 renders the row like the vendored reference.

results.csv records Firefox passing the vendored reference and Chrome and Safari failing it (resvg
passes its own). The failing browsers draw the case differently enough for a visual grade to see;
a grade cannot see the one-pixel band along the clipped edges where Donner and the reference
differ.

PNG SHA-256: `9962099aac41e77fd1d9044fd05d2c939dad0ecfdc941f98dc1b36dae9ab3750`.

To regenerate, delete the file and run from the repository root:

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_default_text -- '--gtest_filter=*complex_order_and_xlink_href*'
```

### `filters/feColorMatrix` identity rows

Disposition: reference disagreement. `donner-feColorMatrix-identity.png` is a Donner CPU rendering
of the corpus file `without-attributes.svg`, at the 500x500 size of the vendored references,
rendered by the resvg suite harness at the commit that adds it. Seven rows share it, because each
draws the same gradient through an identity color matrix: without-attributes, type=matrix without
values or with empty, short or long value lists, and type=saturate and type=hueRotate without a
value. Every backend compares with it at default settings.

An identity color matrix returns its input. Filter Effects 1 gives the conversion between sRGB and
linearRGB as a formula and sets no precision for it, and Donner passes an identity matrix through
without converting; RendererRegressionTests.IdentityColorMatrixLeavesLinearRgbInputUnchanged
checks every identity form against the unfiltered render, independently of this image. resvg
converts filter inputs between sRGB and linearRGB through 8-bit lookup tables, so every dark
channel value in these references below 60 is one of the sRGB levels an 8-bit linear value maps to
(0, 13, 22, 28, 34, 38, 42, 46, 50, 53, 56, 59), verified pixel by pixel, and the smooth gradient
steps: green falls from 13 to 0 where the source runs 12, 10, 9, 8, 7, 6, 5 and on. resvg v0.49.0
uses the same tables.

results.csv records Chrome, Firefox and resvg passing all seven vendored references, and Safari
passing four of them (it fails type=matrix with empty, short and long value lists). Rendered
headless at 500x500, each browser draws all seven rows identically, and the two browsers split.
Chromium 151 matches this golden (0 pixels at the suite's default settings). Firefox 153
reproduces the references' 8-bit linearRGB steps, with the same dark-band green levels, and
differs from this golden by 796 pixels in two single columns.

PNG SHA-256: `da52a1059a66c0ae3155dd5b3b5d5d155e6f6d4b88fab102e538f4183bc04916`.

To regenerate, delete the file and run from the repository root:

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_default_text -- '--gtest_filter=FiltersFeColorMatrix*without_attributes*'
```

### `filters/filter-functions/drop-shadow-function*`

Disposition: reference disagreement; whether to carry Donner renderings for these rows rests on
the weakest evidence among these goldens and is a maintainer decision. `donner-drop-shadow-function.png`
(shared by `drop-shadow-function.svg` and `drop-shadow-function-color-last.svg`, which render
identically), `donner-drop-shadow-function-em-values.png` and
`donner-drop-shadow-function-mm-values.png` are Donner CPU renderings of the unchanged corpus files,
at the 500x500 size of the vendored references, rendered by the resvg suite harness at the commit
that adds them.

Filter Effects 1 (feOffset) says a high quality viewer should interpolate when an offset is a
fraction of a device pixel, and the drop-shadow() function is defined through feOffset.

- `drop-shadow-function` and `color-last` (`blue 4 5 6`): the device offset is (10, 12.5). resvg
  converts the scaled offset to an integer (`dx as i32` in `apply_drop_shadow`), so the vendored
  references, and resvg v0.49.0's renderings, place the shadow at (10, 12), where Donner resamples
  the half pixel.
- `em-values` (`blue 0.2em 0.3em 0.1em` at font-size 64) and `mm-values` (`blue 3mm 4mm 1mm`):
  the device offsets are (32, 48) and (28.35, 37.80). The vendored references place the shadows
  at (30, 47) and (27, 37), consistent with the user lengths truncated to whole units before
  scaling (12.8 19.2 to 12 19, and 11.34 15.12 to 11 15) and the scaled offset truncated; the usvg
  source does not show the length truncation. resvg v0.49.0 renders these two rows differently from
  the vendored references but does not compare with Donner either (1,247 and 1,131 differing pixels
  on the software backend, 1,240 and 1,140 on Geode); for em-values, whose offset is whole, its
  shadow profile across the circle's center row agrees with Donner's within two levels, and the
  remaining differences are in the blur halo.

results.csv records Chrome, Firefox and resvg passing all four vendored references and Safari
failing them. A visual grade sees whether a blurred shadow sits beside the circle; a shift of a
half pixel to two pixels under a blur of 15 device pixels (9.6 for em-values) is not visible to it,
so the browsers passing these references does not show that they truncate.

| Golden | PNG SHA-256 |
| --- | --- |
| `donner-drop-shadow-function.png` | `b5f628d66da78e36f887fedd444eeb9bcd0111b0d62a01ba79ceed9f53405c3c` |
| `donner-drop-shadow-function-em-values.png` | `3bc724817809d4ed92c53c1b655a45f9f3a5a2a031efd4f0f99e48100abd9020` |
| `donner-drop-shadow-function-mm-values.png` | `044fe3cb0fe0176ab7dcbe86831dd49998e68e55d0b77143c2a4ce5a4b04aa3f` |

To regenerate, delete the files and run from the repository root:

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_default_text -- '--gtest_filter=*drop_shadow_function*'
```

## Donner-rendered references

The references below are Donner renderings. Each replaces a vendored reference that encodes a
renderer's own model where browsers measurably agree with Donner instead; a self-rendered golden
guards against regressions but does not prove correctness, so each entry names the independent
evidence. Each was rendered by the full text tier with the basic (TinySkia) backend and compares at
default parameters.

To regenerate one, delete its PNG and run from the repository root (the fixture writes a missing
golden):

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_max -- '--gtest_filter=*<case_with_underscores>*'
```

| Case | Why the vendored reference is not used | SVG SHA-256 | PNG SHA-256 |
| ---- | -------------------------------------- | ----------- | ----------- |
| `text/writing-mode/mixed-languages-with-tb` | Upright U+65E5 and U+672C in Mplus 1p, sideways Latin and Arabic, at 20 user units on the row's 300x300 canvas. CSS Writing Modes 3 section 5.1.1 requires the font's vertical metrics for upright glyphs; Mplus 1p has vhea and vmtx, with a vertical origin of 860 units, which Blink and Gecko take from VORG and then from vmtx, as Donner does. resvg centers upright glyphs on the hhea ascent and descent instead, and the vendored PNG sets both upright glyphs 0.35 to 0.5 device px lower than Donner. Horizontal stroke centers in device px, top to bottom, measured from renders with the corpus fonts loaded through @font-face in headless Chromium 151 and Firefox 153: U+65E5 at 45.43, 55.75 and 66.75 in Donner, 45.49, 55.58 and 66.68 in Firefox, 46.50, 56.50 and 67.74 in Chromium (on whole pixel rows) and 45.79, 56.25 and 67.25 in the vendored PNG; U+672C at 79.00 and 93.39 in Donner, 79.09 and 93.46 in Firefox, 79.09 and 93.45 in Chromium and 79.39 and 93.74 in the vendored PNG. The sideways Latin and Arabic match the vendored PNG within 0.15 px, and all 108 px that differ from it lie on the two upright glyphs. The corpus results mark Chrome, Firefox and resvg as passing and Safari as failing this row. | `3bacf63bdeb8b168a8ca061984e529b6a12d03f8cfa8c163fdbf8a1e92265544` | `b3cd1778a62622798def3373bd7fe7c4e2188f9ba48a85a2c91129fbeb88eda7` |

## Color-adjust filter references

The seven `filters/filter-functions/color-adjust-functions-*` inputs keep their live text and
filters. Their references preserve every original pixel outside the three text bands, including
all seven filtered squares and the frame. The historical PNGs draw heavier labels: switching
Chromium on macOS from `-webkit-font-smoothing: antialiased` to `auto` reduces mismatches
from 461 to 7 against the transparent reference. Firefox’s `-moz-osx-font-smoothing: antialiased`
to `auto` gives 450 to 12 with both images composited on white. Font bytes, layout, filters
and device scale remain fixed within each pair. The original PNG producer is unknown.

usvg 0.49.0 shapes the seven strings with the vendored Noto Sans Regular font
(SHA-256 `6b04c8dd65af6b73eb4279472ed1580b29102d6496a377340e80a40cdb3b22c9`).
The resulting scene contains only vector outlines. TinySkia and Geode each rasterize those
paths at 500 × 500. The labels therefore test Donner’s face selection, shaping and placement
against independent geometry while accounting for each renderer’s glyph-edge coverage.
The filter expectations come entirely from the vendored PNGs.

At 500 × 500, each of the seven fixtures gives the same whole-image mismatch count on macOS
and Linux. The threshold is 0.02 and the ordinary limit is 100 pixels.

| Comparison | CPU simple | CPU full | Geode (Metal and software Vulkan) |
| --- | ---: | ---: | ---: |
| Vendored PNG before the full-tier shaping precision update | 354 | 370 | 375 |
| Outlined-label reference | 25 | 15 | 0 |

In the original qualification on `4a5e9abb`, changing one square from 50% to 45% brightness failed by at least 10,000 pixels. Moving the
labels right by one device pixel failed by at least 1,412 pixels across the two CPU tiers and
native Metal. These comparisons cover the whole image.

The 100% and omitted-value cases share
one PNG per backend, as do 200% and 2. Each pair has identical decoded reference pixels.

### Reproduction

From the repository root, with Python, Pillow, defusedxml and usvg 0.49.0 installed, extract
the common label scene and convert it to paths:

```sh
python3 tools/resvg_test_suite_upgrade/color_adjust_references.py labels /tmp/color-adjust-labels.svg
usvg --skip-system-fonts --use-font-file third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf \
  /tmp/color-adjust-labels.svg /tmp/color-adjust-paths.svg
```

To reproduce the masks, use a disposable checkout of Donner at `4a5e9abb2bd2433ba9566375d7d8ee34ff7cd104`.
In that checkout, replace only `third_party/resvg-test-suite/tests/filters/filter-functions/color-adjust-functions-0percent.svg`
with `color-adjust-paths.svg`. Run the two renderer targets below. The diagnostic comparison to
the full upstream scene fails because the path input contains only the labels. Retain the
`actual_*.png` from the selected test in each target’s `test.outputs` directory. Check for
`[TinySkia]` in the first test log and `[Geode]` in the second.

```sh
bazel test --test_sharding_strategy=disabled --test_arg='--gtest_filter=*color_adjust_functions_0percent' \
  //donner/svg/renderer/tests:resvg_test_suite_default_text
bazel test --test_sharding_strategy=disabled --test_arg='--gtest_filter=*color_adjust_functions_0percent_GeodeGolden' \
  //donner/svg/renderer/tests:resvg_test_suite_geode
```

Copy the two actual PNGs to `/tmp/tinyskia-labels.png` and `/tmp/geode-labels.png`, respectively.
Return to the checkout containing the reproduction script and the unchanged corpus SVGs. The
command writes five PNGs for each backend and checks that the two shared pairs agree byte-for-byte
in RGBA.

```sh
python3 tools/resvg_test_suite_upgrade/color_adjust_references.py compose \
  /tmp/tinyskia-labels.png /tmp/geode-labels.png /tmp/color-adjust-references
```
