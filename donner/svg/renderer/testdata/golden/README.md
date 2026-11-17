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
