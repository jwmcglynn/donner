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

## Donner-rendered textPath/method=stretch reference

`resvg-method=stretch.png` is Donner's own rendering of the unchanged corpus
`text/textPath/method=stretch.svg`. It is used because the vendored PNG does not depict
the feature: it is byte-identical to `text/textPath/simple-case.png`, which is
`method="align"` placement, and the suite's `results.csv` flags every engine except svgnet
as failing the case. [SVG 2 `method="stretch"`](https://www.w3.org/TR/SVG2/text.html#TextPathElementMethodAttribute)
converts glyph outlines into paths whose points are moved along the path's normals, so
glyphs bend with the path.

This reference is a regression guard for Donner's warp, not an independent oracle. It
differs from the vendored align image in 4664 pixels. The warp is pinned by geometry
and renderer tests: `TextEngineTest.TextPathStretchFollowsEachGlyphChord`,
`TextPathStretchKeepsCombiningMarkInBaseClusterFrame` and
`TextPathStretchMappingStopsAtItsOwningRunRange` pin the per-glyph frame;
`TextEngineScriptedTest.StretchTextPathRotatesShapedOffsetsLikeAlign` pins shaped
offsets against align; and `TextPathStretchCurvedGeometryDiffersFromAlign` and the other
`RendererPublicApiTest.TextPathStretch*` cases pin the curved outlines. The independent
check is `RendererPublicApiTest.TextPathStretchOnStraightPathsMatchesOrdinaryRotatedText`,
which requires stretch on a straight path to equal ordinary rotated text.

The image was created from the simple text tier on TinySkia with the suite's
golden-creation path, with the renderer sources of main at
`630d99965cc3d65f05a5c1819b334bbf30cbf133`. The simple text tier matches it with no
differing pixels and the full text tier differs in 3 pixels, within the default
100-pixel budget; the Geode resvg lane skips `text/*` rows through its backend
feature gate.

Corpus SVG SHA-256: `bcd6cbd182b1055635b780924dd32e2021f3a5994177fd616ebd0f98d0c7fc28`.

Reference PNG SHA-256: `ba6c92599b34f0327da5a06536fa4b2f36d8487b83cab4138caa6be03f0e7626`.

To regenerate after an intended rendering change, delete the image, run the
command below from the repository root, and review the new image before
committing it:

```sh
rm donner/svg/renderer/testdata/golden/resvg-method=stretch.png
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run //donner/svg/renderer/tests:resvg_test_suite_impl -- '--gtest_filter=TextTextPath/*method_stretch'
```

## Donner-rendered textPath continuation references

`resvg-mixed-children-1.png` and `resvg-very-long-text.png` are Donner's own renderings of
the unchanged corpus `text/textPath/mixed-children-1.svg` and `very-long-text.svg`. In both,
a `textPath` ends with a space and a green `Text` follows it.
[SVG 2 text on a path layout rules](https://www.w3.org/TR/SVG2/text.html#TextpathLayoutRules)
continue the text after a `textPath` from the end point of the path, and
[CSS Text 3 section 4.1.1](https://www.w3.org/TR/css-text-3/#white-space-phase-1) keeps the
textPath's trailing space on the path and collapses the space after it, so `Text` starts at
the end point. The vendored PNGs continue from the end of the glyphs on the path instead.

These images were regenerated when Donner adopted that collapse rule; before it, Donner kept
the following space and drew `Text` 6.24 user units (15.6 device px) past the end point. Ink
of the green `Text`, in device px at the 500x500 (2.5x) canvas; the Chrome column is a
Chrome 157 rendering of the same SVG with the corpus `NotoSans-Regular.ttf` loaded through
`@font-face`:

| Case             | Path end point | Vendored PNG         | Previous reference | Chrome 157           | This reference       |
| ---------------- | -------------- | -------------------- | ------------------ | -------------------- | -------------------- |
| mixed-children-1 | (450, 250)     | x 419-499, y 146-188 | x 466-499          | x 451-499, y 207-249 | x 451-499, y 207-249 |
| very-long-text   | (250, 250)     | x 290-402, y 210-252 | x 266-378          | x 251-363, y 207-249 | x 251-363, y 207-249 |

`TextEngineScriptedTest.PostTextPathCursorBoundarySpaceStaysOnThePath` pins the collapse
rule. The images were created from the simple text tier on TinySkia with the suite's
golden-creation path, at the collapse rule change. The Geode resvg lane skips `text/*` rows
through its backend feature gate.

| Case             | Corpus SVG SHA-256                                                 | Reference PNG SHA-256                                              |
| ---------------- | ------------------------------------------------------------------ | ------------------------------------------------------------------ |
| mixed-children-1 | `1d9adcd5eff2c995932b4276b9d22713e470ed75c6168deffd855544bca46fcd` | `2ea922a8be96b59e9ed1950eae3bfcda2368710c80a8a6d4a24eb5db6d92a054` |
| very-long-text   | `17eba4ca25ea86b8241dab58d646ce9ea5b64411e8e24ff084b1ea209099aa59` | `6392266f206470a9459514291d4b7b811f3a0b9d406c7fc2d81fdb3ded914021` |

To regenerate after an intended rendering change, delete the images, run the command below
from the repository root, and review the new images before committing them:

```sh
rm donner/svg/renderer/testdata/golden/resvg-mixed-children-1.png donner/svg/renderer/testdata/golden/resvg-very-long-text.png
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run //donner/svg/renderer/tests:resvg_test_suite_impl -- '--gtest_filter=TextTextPath/*mixed_children_1:TextTextPath/*very_long_text'
```
