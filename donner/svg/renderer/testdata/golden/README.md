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
