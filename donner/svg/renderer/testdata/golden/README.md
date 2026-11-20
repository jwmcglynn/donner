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

## Retained textPath reference comparisons

These measurements use the fixture's exact SVG and the suite's Noto Sans Regular face,
with a 500×500 canvas on both macOS arm64 CPU text tiers unless indicated otherwise. The default
comparison uses a 0.02 color threshold and allows 100 mismatched pixels.

| Fixture | Current reference | Simple / full at current settings | Independent resvg v0.49.0 at 300×300, default settings | Independent resvg v0.49.0 at 500×500, default settings |
| --- | --- | ---: | ---: | ---: |
| `closed-path` | Vendored resvg snapshot, default color threshold and 400-pixel limit | 381 / 373 | 147 / 148 | 265 / 263 |
| `side=right` | Vendored resvg snapshot, 0.05 color threshold and 100-pixel limit | 63 / 64 | 2,245 / 2,250 | 7,387 / 7,398 |
| `dy-with-tiny-coordinates` | resvg 0.47.0 | 70 / 71 | 100 / 102 | — |
| `tspan-with-relative-position` | resvg 0.47.0 | 54 / 54 | — | — |
| `tspan-with-absolute-position` | Committed legacy golden | 25 / 30 | 135 / 138 | 183 / 179 |
| `with-underline` | Committed legacy golden | 1 / 3 | 295 / 301 | 182 / 182 |

The vendored `closed-path` reference differs by 381 / 373 pixels at the default color threshold,
inside the existing 400-pixel limit on both macOS and Linux aarch64. The previous 0.1 color
threshold yielded 291 / 286 in the macOS measurements. At 0.02, 307 / 300 mismatches are
within one pixel of a glyph boundary and 73 / 72 lie on the gray guide alone. The remaining black
pixel at (275, 97) has alpha 255 in Donner and 191 in the reference.
SVG 2 limits a single closed subpath to one circuit.
`RendererPublicApiTest.ClosedTextPathAbsoluteXOutsideCircuitHasNoInk` compares placement on a
closed segment against an ordinary positioned glyph and tests the two out-of-circuit directions
against an explicitly hidden glyph. [SVG 2 textPath layout](https://www.w3.org/TR/SVG2/text.html#TextpathLayoutRules).

The vendored `side=right` reference differs by 110 / 112 pixels at the default threshold, above the
100-pixel limit. At 0.025 the same CPU tiers differ by 97 / 99, leaving one pixel of margin on
the full tier. At 0.02, 48 / 50 mismatches are within one pixel of a glyph boundary and 61 / 61
lie on the gray cubic guide alone. The remaining black pixel at (394, 218) has alpha 232 / 234
in Donner and 255 in the reference.
Stable Google Chrome 158.0.8127.37 starts the first glyph at x=180.42 near the path's original end
and advances back to x=173.53. `TextSystemTest.TextPathSideRightReversesTravelDirection`,
`TextPathSideRightMeasuresStartOffsetFromTheOriginalEnd`, and
`TextPathSideRightAppliesToInlinePathGeometry` exercise the reversal on referenced and inline
geometry. [SVG 2 side attribute](https://www.w3.org/TR/SVG2/text.html#TextPathElementSideAttribute).

For the boundary counts, use the red pixels of the default pixelmatch diff. A glyph boundary
is the one-pixel band around the black ink mask (alpha ≥ 128 and all color channels ≤ 32).
For the guide-only count, check for a pixel center within two device pixels of the authored
path, excluding the glyph band. The circle is centered at (250, 250) and has radius 150
device pixels. The right-side fixture uses its two authored cubic segments at 2.5× scale.

### Legacy PNGs

The two committed PNGs below first appear in `0c5402b1d` under the numbered corpus names and
retain their bytes through the rename in `4aef2b23c`. The original renderer and generation
commands are unrecorded. The images serve as regression guards and have separate placement
and paint evidence. Both 300×300 release PNGs and the independently generated 500×500 alternatives
exceed the default allowance. The steps below reproduce the independent reference measurements.

| Golden | SHA-256 |
| --- | --- |
| `resvg-tspan-with-absolute-position.png` | `754eba7cfe125e9ae873187015247f89d7e9f5e8614f604a0ef7880c8c03eaa7` |
| `resvg-with-underline.png` | `3ff550f2200e10a348a24a74aef99196e867fbd4a9249c7c109ea85aca982af2` |

- `tspan-with-absolute-position` exercises absolute distance along a horizontal text path. Chrome
  places the green span's first character at (24.84, 108.77) in SVG user units. On the rendered
  canvas its green-ink centroid differs from the golden by 0.54 px horizontally and 0.07 px
  vertically. `TextEngineScriptedTest.TextPathAbsoluteXPreservesSameGlyphDx` checks the authored
  on-path distance separately. The original snapshot differs from the current renderer by
  603 / 610 pixels at default settings. [SVG 2 text positioning on a path](https://www.w3.org/TR/SVG2/text.html#TextpathLayoutRules).
- `with-underline` exercises decoration paint along the placed text. Donner uses Noto Sans's
  `post` table for the automatic underline offset and width. The font gives a width of 50
  units per 1,000-unit em, which is 1.2 user units at the fixture's 24-unit font size. When
  each rendering is compared against its own undecorated text on the same path, 958 of the
  golden's 1,028 additional dark pixels overlap Chrome's decoration mask. Chrome draws
  2,293 additional dark pixels under that control, a wider band. The default decoration
  thickness is user-agent selected. The original snapshot differs from the current renderer
  by 779 / 788 pixels at default settings. [CSS Text Decoration 4 thickness](https://www.w3.org/TR/css-text-decor-4/#text-decoration-thickness-property).

To reproduce the current legacy comparisons, run these focused cases:

```sh
bazel test //donner/svg/renderer/tests:resvg_test_suite_default_text \
  //donner/svg/renderer/tests:resvg_test_suite_max --test_sharding_strategy=disabled \
  --test_output=all \
  --test_arg='--gtest_filter=TextTextPath/*tspan_with_absolute_position:TextTextPath/*with_underline'
```

For the current-render PNG and the canonical diff, use a scratch checkout and append
`.withMaxPixelsDifferent(0)` to the two legacy comparison parameters. The nonzero
differences cause the test to write its `actual_`, `expected_`, and `diff_` PNGs under
the test's undeclared outputs. The current-render image provides a contemporary
regeneration candidate with known inputs and settings. The committed legacy PNGs
continue to guard the validated placement and underline paint.

To reproduce the independent 500×500 alternatives, run the resvg v0.49.0 release CLI with
only the checked-in Noto Sans face. The direct 300×300 comparisons use the PNGs under
`crates/resvg/tests/tests/text/textPath/` at resvg revision
`66ee0dc7d787d7e0b0ab3085b7849f78838cf2ba`.

```sh
corpus=third_party/resvg-test-suite
for fixture in closed-path side=right tspan-with-absolute-position with-underline; do
  resvg --skip-system-fonts --use-font-file "$corpus/fonts/NotoSans-Regular.ttf" \
    --width 500 --height 500 "$corpus/tests/text/textPath/$fixture.svg" "$fixture.png"
done
```

Capture Chrome at device scale 1 on a 500×500 transparent canvas. Embed the exact SVG
inline and load `NotoSans-Regular.ttf` as the `Noto Sans` family with `@font-face`.
After `document.fonts.ready`, capture the PNG and read the SVG text element's character
positions with `getStartPositionOfChar` and `getEndPositionOfChar`. To isolate the underline,
compare the alpha ≥ 128 black ink against `with-path` after expanding each undecorated
mask by one pixel on each axis. The same fixture path, string, font, and size are used for
both renders.

## Donner-rendered textPath/method=stretch reference

`resvg-method=stretch.png` is Donner's own rendering of the unchanged corpus
`text/textPath/method=stretch.svg`. It is used because the vendored PNG does not depict
the feature: it is byte-identical to the original snapshot’s `text/textPath/simple-case.png`, which is
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

Ink of the green `Text`, in device px at the 500x500 (2.5x) canvas; the Chrome column is a
Chrome 157 rendering of the same SVG with the corpus `NotoSans-Regular.ttf` loaded through
`@font-face`:

| Case             | Path end point | Vendored PNG         | Chrome 157           | This reference       |
| ---------------- | -------------- | -------------------- | -------------------- | -------------------- |
| mixed-children-1 | (450, 250)     | x 419-499, y 146-188 | x 451-499, y 207-249 | x 451-499, y 207-249 |
| very-long-text   | (250, 250)     | x 290-402, y 210-252 | x 251-363, y 207-249 | x 251-363, y 207-249 |

Rendered headless at 500x500 with the suite fonts and compared at the suite's default settings,
Chromium 151 differs from these references by 67 pixels (mixed-children-1) and 31 pixels
(very-long-text), all on glyph edges. Firefox 153 differs by 589 and 1,197 pixels, all at the
green `Text`, because it does not continue the text after a `textPath` from the path's end
point.

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

## Donner-rendered textPath/mixed-children-2 reference

`resvg-mixed-children-2.png` is Donner's own rendering of the unchanged corpus
`text/textPath/mixed-children-2.svg`. It is used because the vendored PNG does not follow
[SVG 2 text on a path layout rules](https://www.w3.org/TR/SVG2/text.html#TextpathLayoutRules),
which continue the text after a `textPath` from the end point of the path: the PNG continues
`long` and the trailing `.` from the end of the glyphs laid out on each path instead. The
suite's `results.csv` flags Chrome and Firefox as failing the case and Safari and resvg as
passing it.

Each `textPath` here ends with a space and is followed by more spaced text.
[CSS Text 3 section 4.1.1](https://www.w3.org/TR/css-text-3/#white-space-phase-1) removes a
collapsible space that follows another one across element boundaries, so the textPath's own
trailing space stays on the path and the text after it starts at the end point. Two browser
behaviors bear on this row, measured headless with the corpus font loaded through `@font-face`:
- Space attribution. Chrome 157 and Firefox 160 both keep the trailing space on the textPath:
  `getNumberOfChars()` is 5 for each textPath and for the green tspan (`very `, `long `,
  `text `).
- Continuation. Chrome 157 starts the text after each path at that path's end point (below).
  Firefox 160 does not: `getStartPositionOfChar` puts the `l` of `long` at user (54.12, 0) and
  the `.` at (49.32, 0), so `long` lands on the top edge and the `.` falls outside the canvas.
  At the suite's default settings, Chromium 151 differs from this reference by 46 pixels, all on
  glyph edges, and Firefox 153 by 1,458 pixels.

Evidence, measured as ink bounding boxes in device px at the 500x500 (2.5x) canvas. The Chrome
column is a Chrome 157 rendering of the same SVG with the corpus `NotoSans-Regular.ttf` loaded
through `@font-face`; Chrome's `getStartPositionOfChar` puts the first character of each run
after a path at exactly that path's end point.

| Ink          | Path end point | Vendored PNG         | Chrome 157           | This reference       |
| ------------ | -------------- | -------------------- | -------------------- | -------------------- |
| green `long` | (200, 250)     | x 189-304, y 270-329 | x 205-320, y 204-263 | x 205-320, y 205-263 |
| trailing `.` | (450, 250)     | x 385-392, y 178-185 | x 454-461, y 242-250 | x 454-461, y 243-250 |

`TextEngineTest.TextAfterTextPathStartsAtGeometricEndpoint` and the
`TextEngineScriptedTest.PostTextPathCursor*` cases pin the continuation point numerically,
including `PostTextPathCursorBoundarySpaceStaysOnThePath` for a space at the boundary, and
`RendererPublicApiTest.PostTextPathCursorOpenEndpointMatchesExplicitTail` requires the
continued text to equal the same text placed explicitly at the end point.

The image was created from the simple text tier on TinySkia with the suite's golden-creation
path, with the renderer sources of commit `913bb7fd2d7872f18ea80236017b6ea9a2ffb0a5`; a
re-render from those sources reproduces it byte for byte. The Geode resvg lane skips `text/*`
rows through its backend feature gate.

Corpus SVG SHA-256: `7db835973f2d650ee3a698f207831e313c95e2973fb70382d4efeabac658941d`.

Reference PNG SHA-256: `15bb577cfdb17641732110e9ba2c3da36ec24b1761ee972d2e5881676fbd61a0`.

To regenerate after an intended rendering change, delete the image, run the
command below from the repository root, and review the new image before
committing it:

```sh
rm donner/svg/renderer/testdata/golden/resvg-mixed-children-2.png
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run //donner/svg/renderer/tests:resvg_test_suite_impl -- '--gtest_filter=TextTextPath/*mixed_children_2'
```

## Gradient strokes on text

The two `painting/stroke/*-gradient-on-text.svg` fixtures draw "Text" in Noto Sans at 60 user
units. The vendored references map the gradients through the glyph ink extents, about
42.80..156.08 by 66.96..110.40 user units. The [SVG 2 bounding-box algorithm][svg2-bbox] uses the
full glyph cells, including each glyph's advance and the font's ascent and descent. At the
rendered text origin the full box is about 42.20..157.40 by 45.66..127.38. The default linear
gradient only sees the horizontal difference. The radial gradient sees both dimensions.

resvg 0.49.0 uses the glyph-cell box. Its 300-pixel renders were compared with Donner both as text
and as the same explicit `usvg` paths with user-space gradient matrices. On the explicit paths,
the two engines agree exactly on the color of 4,769 mutually opaque text pixels. Within each
engine the coverage is identical for the solid and gradient versions of the path.

The earlier 4x4 scan diagnostic still counted 116 differences in each row. Every flagged pixel
was on the curved glyph boundaries and differed by one to four covered samples, allowing one
alpha code for rounding. Holding color constant and substituting the reference coverage reduced
both counts to zero, even with edge pixels included. A separate control used one immutable
expanded stroke for both scans in tiny-skia 0.12.0, the rasterizer used by resvg 0.49.0. Its
internal 4x4 scan allows coarser curve subdivisions than a large aliased render. On the same
sample grid, changing only the subdivision scale to match the internal scan removed all 233
alpha differences larger than one code. A one-code accumulation rounding difference remains.

Firefox 153.0 independently rendered both fixtures at 500 x 500, device pixel ratio 1, using the
suite's Noto Sans Regular font. Firefox 157.0 regenerated both PNGs byte for byte. Its text box was
41.60..158.40 by 45.60..127.60 user units, consistent with glyph cells. The harness compares with
these images at its default 0.02 threshold and 100-pixel allowance. The Geode corpus lane has a
separate feature gate for text.

| Gradient | Simple text | Full text |
| --- | ---: | ---: |
| Linear | 84 | 39 |
| Radial | 92 | 44 |

To reproduce the captures, serve each corpus SVG as an SVG document with a `style` element
registering `Noto Sans` at weight 400 and normal style from the corpus font. Wait for the face to
load and capture the 500 x 500 document over black and white backgrounds in a borderless iframe.
For each pixel, recover alpha as `round(255 - mean(whiteRGB - blackRGB))`, clamped to 0..255.
Divide the black-background channels by `alpha / 255` and round and clamp to get straight RGB.
At alpha zero write RGB zero.

Corpus `fonts/NotoSans-Regular.ttf` SHA-256
`6b04c8dd65af6b73eb4279472ed1580b29102d6496a377340e80a40cdb3b22c9`.

| Gradient | Corpus SVG SHA-256 | Firefox PNG SHA-256 |
| --- | --- | --- |
| Linear | `2f16b9c4f8c9023d3e1cc131514084d27da62e6c76ea918cba379766d2686a62` | `24b2bc34651ec46cdee7e49450e832916b1f0666bbb5cd20e3b543f93f55fdd4` |
| Radial | `aaa4629608d0bba3aab4d50c8be80410f6c5d8038e56dc03c787330e2e61c45b` | `40695b1c5ff590b5367bd645e62dedb9855d4a3a8a7c1bafed16f424fb73dfd1` |

[svg2-bbox]: https://www.w3.org/TR/SVG2/coords.html#BoundingBoxes

## Gaussian blur primitive subregions

The `filters/filter/subregion-and-primitiveUnits=objectBoundingBox-{1,2}.svg`
inputs express the same half-size primitive as a fraction and as a percentage.
The 160-unit object box gives a blur deviation of 1.6 units and a primitive
width and height of 80. Its default origin is the filter region origin (4,4).
`half-box-blur.svg` expresses the region in user units and clips the blurred
rectangle explicitly. The historical PNG stores reduced alpha even in the
opaque interior. Rendering the original input with resvg 0.49.0 also extends
the clip past the expected edge. Chrome 158 and Firefox 157 agree with the
explicit construction. At 800×800 the fixture’s 0.5-unit crosshair spans two
whole pixels, so fractional crosshair sampling does not mask filter regressions.

From this directory, reproduce the shared 800×800 PNG with:

```sh
resvg --width 800 --height 800 half-box-blur.svg resvg-half-box-blur.png
```

For `filters/filter/with-subregion-3.svg`, the first blur is clipped at x=135
on the original 500×500 canvas. The second blur places the half-alpha crossing
at x=135 on the interior scanline y=100 in Chrome, Firefox and resvg 0.49.0.
The historical PNG crosses at x=133.32. `firefox-double-subregion-blur.png`
comes from Firefox 157.0 at 500×500. To reproduce it, give the original root
SVG explicit width and height of 500, decode it as an SVG image, draw it at
(0,0) at its intrinsic size on a transparent 500×500 sRGB HTML canvas, and
export that canvas as PNG. All three tests compare the original SVG inputs
at the default threshold and count limit.

## Nearest-neighbor sampling for `on-feImage`

The `painting/image-rendering/on-feImage.svg` fixture embeds a 64 × 64 PNG and asks for
`image-rendering="optimizeSpeed"`. Its 500 × 500 vendored reference uses a different sampling
grid. The reference introduced in [#1767](https://github.com/jwmcglynn/donner/pull/1767)
samples the embedded PNG independently at destination pixel centers.

To reconstruct `resvg-on-feImage.png`, start with the vendored `on-feImage.png` and decode the
PNG in the SVG's data URL. The default filter region expands the 120-unit square by 10% on
each side. At 2.5 device pixels per unit it occupies the 360 × 360 square starting at (70, 70).
For each integer destination coordinate `70 <= x, y < 430`, copy the embedded RGBA texel at
`floor((x - 70 + 0.5) * 64 / 360)`, `floor((y - 70 + 0.5) * 64 / 360)`. Keep the vendored
pixels outside that square, including the frame.

| SHA-256 input or result | Digest |
| --- | --- |
| Vendored SVG | `2f08caa2e27c3f5911bc14328e63cd9564d79e7e9840b765c36110c403e3d773` |
| Vendored PNG | `94be9637ef48a9996c6d223fdc0187e6df98a0bff125d0f9709601067bb5f5b4` |
| Reconstructed pixels, row-major RGBA8 | `c7dadd180384db409d6ad349be742cd02a7eef087d7be74e48ae9998d167ca9e` |
| Committed golden PNG | `e94b02b863268c3579d36929691e9a8cfb5c389976fe459f00d2f3aa122a4154` |

## Large-coordinate curves at 8x

`tiny_skia_large_coordinate_curves_8x.png` is the CPU (tiny-skia) render of
`../tiny_skia_large_coordinate_curves_8x.svg`: 50 solid and 50 dashed cubic curves around
(4,000,000, 4,000,000), viewed at 8x on a 500 x 500 canvas. It was rendered at main commit
`7e8acc4ab`, before the stroke precision floor existed, so it pins that the floor leaves
this render unchanged.

PNG SHA-256: `045379b1dd1c2f0101bc08df460aba43323f233c41edecfa22300af7bfaae5a6`.

The SVG and the test that renders it are not on that commit, so the recipe copies the SVG out
of the tree first and adds a minimal test. From the repository root, on a commit that has the
SVG, with `<outside>` any directory outside the repository:

```sh
cp donner/svg/renderer/testdata/tiny_skia_large_coordinate_curves_8x.svg <outside>/large.svg
git checkout 7e8acc4ab
cp <outside>/large.svg donner/svg/renderer/testdata/tiny_skia_large_coordinate_curves_8x.svg
cat >> donner/svg/renderer/tests/RendererPublicApi_tests.cc <<'CPP'
#include <fstream>
#include <iterator>
namespace donner::svg {
namespace {
TEST(GoldenRecipe, TinySkiaLargeCoordinateCurves8x) {
  std::ifstream file("donner/svg/renderer/testdata/tiny_skia_large_coordinate_curves_8x.svg");
  ASSERT_TRUE(file.is_open());
  const std::string svg((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  SVGDocument document = ParseDocument(svg);
  RendererTinySkia renderer;
  renderer.draw(document);
  editor::tests::CompareBitmapToGolden(
      renderer.takeSnapshot(),
      "donner/svg/renderer/testdata/golden/tiny_skia_large_coordinate_curves_8x.png",
      "large_coordinate_curves_8x", editor::tests::PixelmatchIdentityParams());
}
}  // namespace
}  // namespace donner::svg
CPP
sed -i.bak 's|data = \["//third_party/public-sans:public-sans_resource_files"\],|data = ["//donner/svg/renderer/testdata", "//third_party/public-sans:public-sans_resource_files"],|' donner/svg/renderer/tests/BUILD.bazel
bazel test -c opt //donner/svg/renderer/tests:renderer_public_api_tests \
  --test_filter='GoldenRecipe.*' --test_env=UPDATE_GOLDEN_IMAGES_DIR=$PWD \
  --sandbox_writable_path=$PWD --nocache_test_results
```

The recipe writes the PNG into the tree; its SHA-256 matches the value above. Discard the
other edits afterwards.

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

### Color emoji bitmap sampling

`text/text/compound-emojis` and `text/text/emojis` use 500 × 500 resvg v0.49.0
renders of the unchanged corpus SVGs with the bundled Noto Color Emoji font. The
first scene draws one joined rainbow flag. The second draws four centered faces.
The full CPU text tier agrees with resvg on the visible color bounds of every
glyph. The remaining differences are compatible with bitmap resampling.

| Fixture | Differences at 0.02 | Differences at 0.10 |
| --- | ---: | ---: |
| `compound-emojis` | 375 | 0 |
| `emojis` | 657 | 0 |

The comparison uses 0.10 and the ordinary 100-pixel allowance. The Linux and
macOS resvg renders are byte-identical, as are the two sets of Donner outputs.
The corpus [BUILD file](../../../../../third_party/resvg-test-suite/BUILD.bazel)
records the reproduction command and checksums. Both fixtures require full CPU
text for the font's color bitmap strikes.

## Donner renderings of `text/font-size/named-value` and `named-value-without-a-parent`

The 500x500 named-size goldens preserve content from the software renderer with simple text at
commit `21c59c75`. For `donner-named-value.png`, the black frame uses independent pixel area
from `third_party/resvg-test-suite/reference-sources/named-value-frame.svg`. The source specifies
an explicit outer rectangle and hole at eight times the final resolution. Averaging each 8x8
block gives the frame coverage. Every pixel outside the frame preserves its original RGBA.
Geode does not render these text rows.

Each row sizes rects of `10em` under the named `font-size` keywords. The em sizes in pixels,
measured from the rect coverage (the SVG sets `font-size="12"` on the group):

| Renderer | xx-small | x-small | small | medium | large | x-large | xx-large |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Donner | 7.2 | 9 | 10.67 | 12 | 14.4 | 18 | 24 |
| Vendored PNG and resvg v0.49.0 | 6.94 | 8.33 | 10 | 12 | 14.4 | 17.28 | 20.74 |
| Chromium 147 and Firefox 153 | 9 | 10 | 13 | 16 | 18 | 24 | 32 |

`larger`, `smaller` and percentages agree in all four renderers (14.4, 10 and 6).

Donner applies the absolute-size table of CSS Fonts 4 section 2.5 (3/5, 3/4, 8/9, 1, 6/5, 3/2 and
2 times `medium`) with `medium` at 12 px. That table is a guideline for user agents, not a
requirement. The references step by a factor of 1.2 per keyword, the CSS 2 table. The browsers
resolve the keywords against their own 16 px `medium`, not the inherited 12 px, with the legacy
integer table (9, 10, 13, 16, 18, 24, 32). `results.csv` records all three browsers passing the
vendored PNGs; that is a visual verdict. Following CSS Fonts 4 here is a maintainer decision.

| Golden | PNG SHA-256 |
| --- | --- |
| `donner-named-value.png` | `52f758743a31018c94d2b16df37bdd4e8cf5c1b59237358a18c5f8e5f6234c12` |
| `donner-named-value-without-a-parent.png` | `65f5aa8f898bdb0f1586dd47b3db48ab830af39fe0cee350a8b01ac27e499e16` |

### Reproduce the named-value frame hybrid

1. Export the original font-policy PNG. Its Git blob is
   `59d801f5a33b0eaac75c16e1167245d848c2c0a3` and its PNG SHA-256 is
   `7e65b966683f6daad8a6b931d67bc8a61be0de4a29f606071b3967cfa22fd3cf`.

   ```sh
   git cat-file blob 59d801f5a33b0eaac75c16e1167245d848c2c0a3 > font-policy.png
   ```

2. Render the independent frame source with resvg 0.49.0 from the repository root.
   The resulting 4000x4000 image has fully transparent or fully opaque pixels.

   ```sh
   resvg --width 4000 --height 4000 \
     third_party/resvg-test-suite/reference-sources/named-value-frame.svg frame-8x.png
   ```

3. Decode `font-policy.png` as straight RGBA and `frame-8x.png` as 8-bit alpha.
   For output pixel `(x,y)`, sum the 64 alpha bytes in columns `8*x` through
   `8*x+7` and rows `8*y` through `8*y+7` of the frame image. Set
   `coverage = (sum + 32) // 64`. Replace the output pixel with
   `(0, 0, 0, coverage)` when coverage is positive. Preserve all four original
   bytes everywhere else. This changes 2,988 output samples exclusively in the
   frame. Encode the resulting 500x500 bytes as straight 8-bit RGBA. The checked-in
   PNG uses filter 0 on every row and one IDAT deflated at level 6 with zlib 1.2.12,
   between IHDR and IEND.

### Reproduce the without-a-parent golden

Remove only `donner-named-value-without-a-parent.png` and run from the repository root.

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_default_text -- \
  '--gtest_filter=*TextFontSize*named_value_without_a_parent'
```

## Inline text decoration and malformed lists

The inline cases compare at 800×800. The half-unit crosshair covers two whole pixels at
this size. The original SVGs exercise a valid space-separated decoration list and reject
comma-separated and concatenated forms. The malformed inputs render identical undecorated
text in resvg v0.49.0 and share one reference.

The literal multiline scene uses Noto Sans Regular's 2.4-unit underline and strike
thickness at 48 units. HarfBuzz gives the run an advance of 92.16 units. The underline
starts at y=103.6, the adjusted overline at y=51.088, and the adjusted strike at y=80.944.
The overline and strike positions exercise Donner's multiline spacing policy. The scene
draws black fill with green stroke, with the underline and overline below the glyphs and
strike above. A control using unadjusted positions differs by more than 13,800 pixels at
default settings.

Run from the repository root with the resvg v0.49.0 release CLI:

```sh
corpus=third_party/resvg-test-suite
golden=donner/svg/renderer/testdata/golden
resvg --skip-system-fonts --use-font-file "$corpus/fonts/NotoSans-Regular.ttf" --width 800 --height 800 \
  "$golden/all-types-inline-bands.svg" "$golden/resvg-all-types-inline-bands.png"
resvg --skip-system-fonts --use-font-file "$corpus/fonts/NotoSans-Regular.ttf" --width 800 --height 800 \
  "$corpus/tests/text/text-decoration/all-types-inline-comma-separated.svg" \
  "$golden/resvg-all-types-inline-no-decoration.png"
```

| Reference | SHA-256 |
| --- | --- |
| resvg-all-types-inline-bands.png | 4a9f8b2def1a673e91396d9eb64ff2e71aaada40ad5cb616ae1e56d3132ca911 |
| resvg-all-types-inline-no-decoration.png | 2550df9e39e1e949b633b924c98995203e5bc5c134bfb9537cf8a9868e2082f7 |

The nested-span test compares its 500×500 archived reference at a 0.09 color threshold.
The underline takes its yellow fill and green stroke from the declaring child span.
A literal 800×800 scene preserves both paints. Its supersampled reference rounds 20%
coverage to roughly 25%, producing a full-row difference against analytic rectangle coverage.

The rotated underline uses its 500×500 archived reference at a 0.03 color threshold.
Over 4,000 common opaque gradient samples measure a wider horizontal gradient across the
rotated glyphs in the archive. The effective widths are 246.5 device pixels in the archive
and 230.8 in Donner. Both tests keep the ordinary 100-pixel limit.

## Cyrillic combining mark position

`complex-graphemes-and-coordinates-list.svg` positions the decomposed Cyrillic
short i at `x="87" y="100 120"`. HarfBuzz forms one glyph from the two codepoints.
The independent scene `cyrillic-first-y.svg` uses the equivalent precomposed glyph
at `x="87" y="100"`, with the same frame and crosshair. With the bundled Noto Sans
Regular face, the precomposed scene and the original decomposed scene with only
the first y value produce byte-identical PNGs in resvg v0.49.0.

At 800 × 800, the half-unit crosshair covers two whole pixels. The unchanged
corpus fixture compares against the independent scene in full CPU text at the
ordinary 0.02 threshold and 100-pixel allowance. Rendering the unchanged list
directly with the release CLI provides a negative control that misplaces the
interior mark. Placing the precomposed glyph at y=120 provides a second control.

On macOS and Linux full CPU text, the independent reference differs by one
pixel. The negative interior-y reference differs by 1,625 and the y=120
cluster reference differs by 8,846 at the same settings.

Run from the repository root with resvg v0.49.0:

```sh
corpus=third_party/resvg-test-suite
golden=donner/svg/renderer/testdata/golden
resvg --skip-system-fonts --use-font-file "$corpus/fonts/NotoSans-Regular.ttf" --width 800 --height 800 \
  "$golden/cyrillic-first-y.svg" "$golden/resvg-complex-graphemes-and-coordinates-list.png"
resvg --skip-system-fonts --use-font-file "$corpus/fonts/NotoSans-Regular.ttf" --width 800 --height 800 \
  "$corpus/tests/text/text/complex-graphemes-and-coordinates-list.svg" cyrillic-bad-interior-y.png
```

| Reference input or output | SHA-256 |
| --- | --- |
| Corpus SVG | `95fe1532a1e6fc1fb3dc78d9771effb6da9d788e3a5c644aef57984c09d9e8e6` |
| Noto Sans Regular | `6b04c8dd65af6b73eb4279472ed1580b29102d6496a377340e80a40cdb3b22c9` |
| `cyrillic-first-y.svg` | `5bc36b1f85524d5bd9620fff0b9ccc162c237f51c60b9884efd70042786ca5aa` |
| Independent reference PNG | `0c37edfeafdd94c8a9b8414f1c5794fee897b933a0dbd6a3e1ad8087bcba6b83` |
| Negative interior-y PNG | `8e853b11b3046cdba0497242e513bd0f641f27b09a33f4e2058666e66e8f2c39` |


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

## Donner renderings of text references with an opaque background

The vendored PNGs for the rows below are opaque white, while Donner's canvas and most vendored
references are transparent. Under the comparator's checkerboard blending that
background alone counts about 235,000 pixels. No resvg v0.49.0 reference fixes them: the
upstream renderings at that release draw something different (see each row). The goldens are
Donner renderings at commit `21c59c75`, made with the software renderer and simple text at
default settings. Full text matches them within 1 pixel (0 on `coordinates-list`, 1 on each
`with-path` row). Geode skips these rows ("Geode backend does not support text rendering"). A self-rendered golden cannot prove itself correct, so each row
records the independent evidence.

### `text/text-anchor/coordinates-list`

`donner-coordinates-list.png` (500x500). The red `T` plus `<tspan y="120">ext</tspan>` must be
covered by the black `Text` with `y="100 120"`. In both texts, `ext` starts a new anchored chunk
because a `y` value alone starts one (SVG 2, Text layout algorithm, "Resolve character
positioning": the chunk count is the larger of the `x` and `y` list lengths). Anchoring is
applied after positioning ("Apply anchoring"), so `ext` is centered on its unanchored start,
the end of the unshifted `T`. Three pieces of evidence agree with Donner:
- Glyph placement. Flattened onto white, Donner's rendering matches the vendored PNG's glyph
  placement: the `T` ink centroid is identical, and the `ext` centroid is 0.36 px to the right.
  results.csv records Chrome and Safari passing that reference.
- Browsers. Rendered headless at 500x500 with the suite fonts, Chromium 151 differs from the
  golden by 5 pixels at the suite's default settings, all on glyph edges. Firefox 153 differs by
  735 pixels: it places the black `T` and `ext` so that the red text shows beside them, so it
  anchors the `y`-only chunk differently (results.csv also records Firefox failing the row).
- Glyph edges. Flattened, Donner's analytic rendering still differs by 406 pixels (simple text)
  and 408 (full text), all on the glyph edges of `T` and `ext`, where the red text shows through
  the anti-aliased edges of the black text. That is edge coverage only: Donner rendered at 4x
  size, box-filtered 4x4 down to 500x500 and flattened onto white differs from the vendored PNG
  by 16 pixels (simple text) and 15 (full text), inside the default allowance.

resvg v0.49.0 centers `ext` on the end of the already anchored `T`, about 6 user units further
left, and Donner differs from it by 245 pixels at 300x300.

PNG SHA-256: `f036660ed5d2f21cbbedbbba1f04c8aa94f31cc33958a3dd63e5cf13bb52f5bb`.

### `text/textPath/with-path` and `with-path-and-xlink-href`

Both rows compare against `donner-textPath-with-path.png` (500x500). Their text follows the
SVG 2 `path` attribute of `textPath`, which takes precedence over `href` (SVG 2, Text,
"The 'path' attribute"), so the two files draw the same picture. Flattened onto white, Donner's
rendering differs from each vendored PNG by 38 pixels (simple text) and 33 (full text), inside
the default allowance. Rendered headless at 500x500 with the suite fonts, Chromium 151 and
Firefox 153 both follow the `path` attribute over `href`, and differ from the golden by 47 and
38 pixels at the suite's default settings, all on glyph edges (15 pixels between the two
browsers). results.csv records Firefox passing the vendored PNGs and Chrome and Safari failing
them; Chromium 151 passes, so the Chrome grade predates its `path` support. resvg v0.49.0 does
not implement the attribute: its references draw only the guide path and frame, and results.csv
records resvg failing both rows.

PNG SHA-256: `8e17141e31f90ea34371c432c95e7aa5870d7e068c76eece8c8a2190fa85f2d7`.

To regenerate one of these goldens, delete the file and run this from the repository root,
with the filter naming the row:

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_default_text -- '--gtest_filter=*TextTextPath*with_path*'
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

### Filter input clipping and a narrower primitive subregion

The three unchanged corpus fixtures use the ordinary 0.02 threshold and
100-pixel allowance. Their adjacent archival PNGs remain available. In flat
regions the archives give (0, 113, 49, 209) for the authored opaque seagreen.
Compositing that color over white nearly reproduces the authored color. On a
transparent background it does not preserve the alpha of a constant blur input.

The [SVG 1.1 filter template rule](https://www.w3.org/TR/SVG11/filters.html#FilterElementHrefAttribute)
inherits absent attributes from the referenced filter. It inherits filter
primitives only when the referring filter defines none. Both `xlink:href`
fixtures therefore resolve to the same sigma-4 blur and region
`[36,164) × [52,212)`. The first inherits all bounds and the primitive. The
second inherits its missing bounds and supplies its own sigma-4 primitive.
The witness draws the original brush's intersection with the filter region,
`[36,164) × [52,180)`, applies a padded blur, then clips the result to the region.
Both rows use that image.

For `with-region-and-subregion`, the filter's left edge is 49.6 in user units.
The witness clips the input at that edge before blurring. The 100-unit primitive
width clips the result at 149.6, equivalent to device x=374 at 500px. Alpha at x=373
remains opaque on the midline. Treating the primitive's right edge as an input
clip would create a blur ramp there and fails by more than 6,000 pixels.

The reference images use the resvg 0.49.0 release CLI at 500x500, with the upstream
MIT notice retained. Direct renders of the original SVGs by the same release
produce too little alpha at the input clip. On a straight edge, the first
interior pixel has alpha 72 rather than the approximately half-covered blurred
edge at 132. A control that applies the clip only after the blur produces 255
there. The explicit clipped-input witness follows the Gaussian profile within one
alpha code on the measured left-edge cross section. Firefox 157 matches the
witness under the ordinary comparator. On these SVG 1.1 `xlink:href` fixtures,
Chrome 158 returns an empty filter result for the all-inherited case and uses
different bounds for the filter with one local primitive.

```sh
cd donner/svg/renderer/testdata/golden
resvg --width 500 --height 500 xlink-filter-bounds.svg resvg-xlink-filter-bounds.png
resvg --width 500 --height 500 filter-input-and-subregion.svg resvg-filter-input-and-subregion.png
```

| Golden | PNG SHA-256 |
| --- | --- |
| `resvg-xlink-filter-bounds.png` | `9ebb7922a3da396ec9a892db7850c929acf2f1af071c7a0db672e38be1ad18e9` |
| `resvg-filter-input-and-subregion.png` | `f999add4a549c79697a68a2995f7d59b3cc2f09b69f4dfba8ae44058b7f3296f` |

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

### `filters/feSpotLight/complex-transform`

The 500x500 shared reference `resvg-complex-transform.png` uses the documented
renderer convention for this skewed light. The independent model computes the
radial source height and Sobel normals, the device-space light vector, the
user-space soft cone, and the outer frame. Its only inputs are the fixture's
geometry and the documented convention for cone attenuation.

```sh
python3 tools/resvg_parity/spotlight_reference.py /tmp/spotlight-reference.png
python3 tools/resvg_parity/spotlight_reference.py --hard-cone /tmp/spotlight-hard-cone.png
```

At the ordinary 0.02 color threshold and 100-pixel allowance, the soft model
counts zero differences against the shared PNG and 42,914 against the adjacent
archive. All 250,000 pixels agree with the shared PNG within one visible RGBA
code. Removing the cone fade counts 5,859 differences against the shared PNG.
This control qualifies the selected renderer convention for this fixture under
skew.

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

## Donner renderings of disputed rows

Each golden below is a Donner rendering of the unchanged corpus file, at the size of its vendored
reference, written by the resvg suite harness with the software (TinySkia) backend and simple text
at default settings, from the code of the change that adds it. Every lane that runs a row compares
it at default settings (0.02 threshold, 100 pixels): the simple and full text tiers, and Geode for
the rows outside `text/`, whose differences are listed per row. A self-rendered golden cannot prove
itself correct, so each row states the normative text or harness condition that decides it, what
the vendored PNG gets wrong or was made under, and an independent contract test that does not
read the golden or the corpus file.

To regenerate one, delete its PNG and run this from the repository root, with the filter naming
the row (for example `*FiltersFilter*invalid_FuncIRI*`):

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_default_text -- '--gtest_filter=*<Suite>*<case_with_underscores>*'
```

### `filters/filter/invalid-FuncIRI`

`donner-invalid-FuncIRI.png` (500x500). The rect's `filter="url(#filter1)"` names an element that
does not exist. Filter Effects 1, section 5 (the `filter` property, `<url>` entry), says that for a
reference to a non-existent object or to an element that is not a `filter`, "the whole filter
chain is ignored. No filter is applied to the object." The rect therefore draws unfiltered. The
vendored PNG hides it: the 160x160 square is fully transparent, and only the frame is drawn. The
corpus paints the rect red, its usual failure color, yet the specification requires it to be
visible, so the golden shows a red square. The suite's results.csv records Chrome, Firefox and
Safari as passing the hidden-rect PNG; those are visual grades of the vendored image, and the
Filter Effects text above decides the row. Geode differs from the golden by 0 pixels.

Independent evidence: `FilterReferenceStages.InvalidReferenceDisablesEntireChain` renders a red
rect under `url(#missing)` and four mixed chains and requires each to match the same rect drawn
without a filter.

### `filters/filter-functions/one-invalid-url-in-list`

`donner-one-invalid-url-in-list.png` (500x500). The list `grayscale(0.5) url(#missing)
opacity(0.5)` contains a reference to a missing element, so by the same Filter Effects 1 section 5
rule ("the whole filter chain is ignored") no filter function in the list applies, and the square
keeps its opaque seagreen (46, 139, 87). The vendored PNG applies the functions around the missing
reference: its square is (80, 126, 100) at alpha 127. The suite's results.csv records Chrome as
passing that PNG and Firefox and Safari as failing it; the specification text decides the row.
Geode differs from the golden by 0 pixels.

Independent evidence: `FilterReferenceStages.InvalidReferenceDisablesEntireChain` (including
`opacity(0.25) url(#missing) invert(1)`) and `InvalidReferenceLeavesGroupChildrenVisible`.

### `filters/filter-functions/url-and-grayscale`

`donner-url-and-grayscale.png` (500x500). The chain is a blur from a `filter` element followed by
`grayscale()`. Filter Effects 1, section 5, says that `color-interpolation-filters` has no effect
on filter functions, which "must operate in the sRGB color space". grayscale() of seagreen in sRGB
is 0.2126 x 46 + 0.7152 x 139 + 0.0722 x 87 = 115.47, and the golden's interior is 115. The
vendored PNG's interior is 122, the result of applying the function to linearRGB values. The
suite's results.csv records Chrome and Firefox as passing that PNG and Safari as failing it; a
visual grade cannot tell 115 from 122, and the specification text decides the row. Geode differs
from the golden by 0 pixels.

Independent evidence: `RendererRegressionTests.CssFilterContractMixedUrlGrayscaleUsesSrgb`
requires `url(#identity) grayscale()` over seagreen to equal a solid rgb(115 115 115) rect.

### `masking/clipPath/circle-shorthand-with-stroke-box` and `circle-shorthand-with-view-box`

`donner-circle-shorthand-with-stroke-box.png` and `donner-circle-shorthand-with-view-box.png`
(500x500). Disposition: harness mismatch. Both vendored PNGs are opaque white (no pixel has alpha
below 255), while the suite renders on a transparent canvas, so the comparisons differ by 147,190
(`stroke-box`) and 88,392 (`view-box`) pixels, nearly all of them background. The geometry is the
same: flattened onto white, no pixel of either golden is more than 46 levels from the vendored
PNG. With `stroke-box`, `circle()` takes the
closest side of the 140-unit stroke box of the 120-unit rect with a 20-unit stroke, a circle of
radius 70 at (100, 100); with `view-box` it takes the 200-unit viewBox, radius 100. Geode differs
from each golden by 0 pixels.

Independent evidence: `CircleClipPath.DefaultAndStrokeBoxIncludeStrokeGeometry` and
`CircleClipPath.ViewBoxUsesNearestSvgViewport` compare each clip with an explicit `<circle>` clip
path of the expected radius.

### `structure/svg/no-size`

`donner-no-size.png` (500x500). Disposition: API policy. The root `svg` has no `width`, `height`
or `viewBox`, so it has no intrinsic size (SVG 2 section 8.12), and SVG 2 leaves the initial
viewport of such a document to its host. Donner's API gives it the caller's canvas, here the
suite's 500x500, at one user unit per px (section 8.4): the 160-unit green square covers device
(20, 20) to (180, 180) inside the 198-unit frame, and the rest of the canvas is transparent. The
vendored PNG was made with a different host viewport: it shows the same artwork in a 200-unit
viewport scaled 2.5 times, the suite's usual scale, with the origin kept (pixel 0 transparent,
the frame from about device px 1, the square from px 50 to 450). Both follow the specification;
they differ in the initial viewport the host supplies. The suite's results.csv records Chrome,
Firefox, Safari and resvg as passing the PNG. Geode differs from the golden by 7 pixels.

Independent evidence, which asserts Donner's API contract:
`RendererRegressionTests.ReferenceContractUnsizedRootUsesCallerCanvasWithoutContentFit` requires
an unsized root rendered on a caller canvas to match the same rect drawn at its user-unit
position, with transparent pixels outside it.

### `structure/use/xlink-to-an-external-file`

`donner-xlink-to-an-external-file.png` (500x500). Disposition: harness mismatch plus reference
glyph rasterization. The `use` references `#text1` in `resources/simple-text.svg`, a `text`
element whose own document sets Arial at 64 units on its root. The referenced element is cloned
without its ancestors, so it inherits `fill="green"`, Noto Sans and 64 units from the `use`. The
vendored PNG draws the same text but is opaque white (no pixel has alpha below 255), while the
suite renders on a transparent canvas, so the comparison differs by 232,379 pixels, nearly all of
them background. Flattened onto white, the golden still differs from the PNG by more than 5 levels
on 2,117 pixels (at most 216 levels), all of them on the glyphs: the centroids of the three glyph
clusters (`T`, `ex`, `t`) agree within 0.35 device px, but the PNG's glyph ink is about 5.6%
heavier, consistent with stems about 0.6 device px wider. That residual is the reference's glyph
rasterization, not glyph placement. Geode differs from the golden by 0 pixels.

Independent evidence:
`RendererPublicApiTest.ExternalUseTextFragmentUsesHostStyleAndTransparentCanvas` requires a
`use` of an external `text` fragment to take the host's fill and font size, ignore the external
document's root styles, keep the canvas transparent, and match the same text written inline.

### `text/font/simple-case`

`donner-font-simple-case.png` (400x400, the size of the vendored PNG). The `text` sets only
`font="bold italic 64 Noto Sans"`. SVG 2 section 6.6 lists the presentation attributes, and the
`font` shorthand is not among them, so the attribute is ignored and the text draws in the initial
font: the initial `font-family`, `serif`, which the suite harness maps to Noto Serif, at the initial
font size. The vendored PNG applies the attribute as SVG 1.1 did and draws bold italic Noto Sans
at 64 units. Geode does not run `text/` rows.

Independent evidence: `RendererRegressionTests.ReferenceContractFontAttributeIsIgnored` requires
text with a `font` attribute to match the same text without it.

### `text/textPath/writing-mode=tb`

`donner-textPath-writing-mode-tb.png` (300x300, the size of the vendored PNG). Upright Mplus 1p
ideographs and kana on a curved path at 24 units. CSS Writing Modes 3 section 5.1.1 requires upright
glyphs to use the font's vertical metrics, and Mplus 1p has `vhea` and `vmtx`: every glyph here
advances 1000 units and has its vertical origin 860 units below the top of its cell. SVG 2 section
11.8.3 (text on a path layout rules) places each glyph on the path by that vertical advance. The
vendored PNG puts upright glyphs elsewhere; it differs from Donner by 219 pixels (simple text) and
221 (full text). Both text tiers compare with the golden at default settings; Geode does not run
`text/` rows.

Independent evidence: `VerticalTextPathPrecision.CurvedCjkUsesIndependentArcFrames` renders this
path and text and requires it to match each glyph drawn as horizontal text in an independently
computed frame on the curve, after checking the 1000-unit advances and 860-unit origins in the
font's tables, and `TextPathConvergence.UprightCjkUsesFontVerticalOriginsOnPathsAndInFlatText`
requires upright text on a straight path to match the vmtx placement.

| Golden | PNG SHA-256 |
| --- | --- |
| `donner-invalid-FuncIRI.png` | `e1409e0b459dfb962a6c6a38ffe82fdaa57a4435a016696fb85ff0555558a50f` |
| `donner-one-invalid-url-in-list.png` | `383d16d1523001752ac353ce8875e1b900a1efa3561eb95fab9fb1d7a0ad598a` |
| `donner-url-and-grayscale.png` | `934860bcb56d1f714c9aa5137ff13c52280258c9b9c5bf91ced54ed974db60b6` |
| `donner-circle-shorthand-with-stroke-box.png` | `5bddc6ef292d0970c3571dead6e4d6a7b9588329d86183c633a7632f9915c3a3` |
| `donner-circle-shorthand-with-view-box.png` | `bc60330d7e98ea0a01d40667ba2802d78b9a89e297d555431cffb11ac2fef1c3` |
| `donner-no-size.png` | `d999792b03ca8bb92b6bf440e030ea9f2b1ff803c0bd1567e34ebf45844c9d2a` |
| `donner-xlink-to-an-external-file.png` | `400dc754087e84efd15a428ad9671bcc34f808eccd147cb59231bd4bcba41565` |
| `donner-font-simple-case.png` | `f4a40c7b0fe7dbc476eda3ce0fff92a35a00a38249b14dafa22625ba1120473c` |
| `donner-textPath-writing-mode-tb.png` | `53f79bd0f51dea2d8499227ac55958e2e1bf3c32f5f124ad50018d4d95e88b1c` |

## Donner-rendered references

The references below are Donner renderings of vertical text. Each replaces a vendored reference
that does not follow CSS Writing Modes 3 section 5.1.1, which requires upright glyphs to use the
font's vertical metrics and vertical font features ("the OpenType vert feature must be enabled").
A self-rendered golden guards against regressions but does not prove correctness, so each entry
names the independent evidence: the font's own tables, the contract test
`TextPathConvergence.UprightCjkUsesFontVerticalOriginsOnPathsAndInFlatText` (upright Mplus 1p
glyphs at their vmtx origins and 1000-unit advances, compared with horizontal text placed from
those numbers alone), and browser measurements where they were taken. Each was rendered by the
full text tier with the basic (TinySkia) backend from the code of the change that added it,
regenerates byte for byte from that code with the command below, and compares at default
parameters.

To regenerate one, delete its PNG and run from the repository root (the fixture writes a missing
golden):

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_max -- '--gtest_filter=*<case_with_underscores>*'
```

| Case | Why the vendored reference is not used | SVG SHA-256 | PNG SHA-256 |
| ---- | -------------------------------------- | ----------- | ----------- |
| `text/writing-mode/japanese-with-tb` | Upright U+65E5 and U+672C in Mplus 1p at 64 user units (160 device px per em). Mplus 1p has vhea and vmtx: both glyphs advance 1000 units, and each glyph's top side bearing plus its yMax puts the vertical origin 860 units below the top of its cell, so the ink tops belong at device y 88.92 and 241.88; Donner's glyph ink starts on rows 89 and 242. The vendored PNG sets the glyphs 5.55 and 5.70 device px lower (best-fit translation of each glyph's ink), about 35 units. That placement follows neither the font's vertical metrics nor an origin centered on the hhea ascent and descent, which at 877.5 units would put the glyphs only 2.8 px lower. Caveat: the corpus file writes font-family="Mplus 1p" unquoted, which is invalid CSS because of the 1p token, so browsers fall back to a default font, and the corpus results' browser passes for this row reflect that fallback; Chrome 156, given a copy that differs only by quoting the family name, places the glyphs within 0.25 px of Donner. | `a7cc22f6a69b9cf4c4025372bbd0bf167f6d9a31c31e515db0375f30521f020f` | `f750e889461dea59c46ceb3945aac85f06ff5baa312ad7fb5b9300dc5e291812` |
| `text/writing-mode/mixed-languages-with-tb` | Upright U+65E5 and U+672C in Mplus 1p, sideways Latin and Arabic, at 20 user units on the row's 300x300 canvas. CSS Writing Modes 3 section 5.1.1 requires the font's vertical metrics for upright glyphs; Mplus 1p has vhea and vmtx, with a vertical origin of 860 units, which Donner uses (Blink and Gecko also take the origin from VORG and then from vmtx). The vendored PNG sets both upright glyphs 0.35 to 0.5 device px lower than Donner. Horizontal stroke centers in device px, top to bottom, measured from renders with the corpus fonts loaded through @font-face in headless Chromium 151 and Firefox 153: U+65E5 at 45.43, 55.75 and 66.75 in Donner, 45.49, 55.58 and 66.68 in Firefox, 46.50, 56.50 and 67.74 in Chromium (on whole pixel rows) and 45.79, 56.25 and 67.25 in the vendored PNG; U+672C at 79.00 and 93.39 in Donner, 79.09 and 93.46 in Firefox, 79.09 and 93.45 in Chromium and 79.39 and 93.74 in the vendored PNG. The sideways Latin and Arabic match the vendored PNG within 0.15 px, and all 108 px that differ from it lie on the two upright glyphs. The corpus results mark Chrome, Firefox and resvg as passing and Safari as failing this row. | `3bacf63bdeb8b168a8ca061984e529b6a12d03f8cfa8c163fdbf8a1e92265544` | `b3cd1778a62622798def3373bd7fe7c4e2188f9ba48a85a2c91129fbeb88eda7` |
| `text/writing-mode/tb-and-punctuation` | Mplus 1p kana, ideographs and the punctuation U+3001 and U+3002 at 16 user units on the row's 300x300 canvas. CSS Writing Modes 3 section 5.1.1 requires the vertical font features for upright typesetting, naming the OpenType vert feature, and the vertical metrics. Donner shapes vertical runs with vert enabled and draws the comma and full stop vertical forms upright, right of the column's center (ink centers at device x 157.5 and 156.9, column center x 150). The vendored PNG has no vert forms: it sets the comma sideways, its horizontal form rotated a quarter turn (its stroke runs from lower left to upper right, where the upright form runs from upper left to lower right), and both marks sit left of the center (x 142.8 and 142.7), 14.7 and 14.1 px from Donner's. Its other 10 ink clusters agree with Donner within 0.75 device px. Chrome 156 places the punctuation forms where Donner does and agrees with it within 0.4 px on all 12 ink clusters. 228 px differ. The corpus results mark Chrome and Firefox as passing and Safari and resvg as failing this row. | `100f4e24b2fd7013e554303f71b9b63199789863e81e86f8b046f340c6e34028` | `808ecdad9093a3ae53788d51c85da384f842eee74ba6bf439c80e035e4caf6b7` |
| `text/alignment-baseline/hanging-on-vertical` | alignment-baseline: hanging in vertical text, upright U+65E5 and U+672C then sideways "Japan" in Mplus 1p at 24 user units (60 device px per em). The vendored PNG applies the same cross-axis hanging shift as Donner: matched in reading order, the horizontal centers of all 7 ink clusters agree within 0.65 device px. The difference is along the column. The PNG advances the upright U+65E5 by about 1.39 em (83.1 device px to U+672C), where the font's vmtx advance is 1000 units, 1 em; Donner advances 59.9 px. So U+672C sits 23.1 px and the sideways letters about 45 px further down in the PNG, while the letters' own advances agree within 0.6 px. CSS Writing Modes 3 section 5.1.1 requires the vertical metrics for upright glyphs, and the contract test above checks the 1000-unit advance. Chrome 156 also applies the hanging baseline and agrees with Donner within 1.1 device px on all 7 ink clusters. The simple text tier differs from this full-tier rendering by 10 px. | `cb9c38a6b0542998f1be4ff2efbc14cd39eb656f4e29547542d65bebca6378d6` | `7adf76966bf9ae515051db400b4f29bd5f61952c8c37acd2b2ff2db4715e398d` |

## Explicit tile geometry for nested and skewed patterns

The two 500×500 references use resvg 0.49.0 with explicit SVG tiles. The
comparison still renders each original pattern SVG at its original size.

- `out-of-order-referencing` fills a 160×70 rectangle with 24×21 user-unit
  tiles. The nested pattern in the lower rectangle fills a 10×10 object box
  with 1.5×3 tiles. Its green rectangle crosses the tile boundary, leaving
  only 0.5 units of its width visible per tile. `nested-rectangles.svg`
  expands both grids with rectangles and clips their outer bounds. Rendering
  the original with resvg 0.49.0 also rounds the upper 52.5-device-pixel
  period; simply taking that PNG would preserve a different error.
- `text-child` clips the border and text to each 40×25 cell, then skews the
  grid by 10 degrees. `skewed-text-tiles.svg` applies these operations
  explicitly and clips the result to the original rounded rectangle.

Chrome Stable 158 and Firefox 157 differ from the explicit grids at fractional
boundaries. In the nested case, the archival PNG also disagrees on fully opaque
and empty interiors. Moving the nested cells by 0.5 user units or removing the
text-grid skew produces thousands of differences against the original Donner
renders at the ordinary settings.

From this directory, reproduce both references with:

```sh
resvg --width 500 --height 500 nested-rectangles.svg resvg-nested-rectangles.png
resvg --skip-system-fonts --use-font-file ../../../../../third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf \
  --width 500 --height 500 skewed-text-tiles.svg resvg-skewed-text-tiles.png
```

## Donner renderings of recursive pattern references

`paint-servers/pattern/recursive-on-child` compares against `donner-pattern-recursive-on-child.png`.
`self-recursive` and `self-recursive-on-child` render identically and share
`donner-pattern-self-recursive.png`. All three are 500x500 renderings by the software renderer
with simple text at default settings. Full text and Geode on Linux lavapipe (aarch64) render all
three identically (0 pixels counted).

Each tile is a 40 x 25 unit rect whose 1-unit stroke straddles the tile edge, so each tile
clips half of its stroke and two neighboring tiles draw the two halves. In the self-recursive rows
the rect's fill references its own pattern, a circular reference, so that fill layer is not
painted (SVG 2 Linking, "circular references"; Painting, "Specifying paint": an invalid paint
server reference without a fallback paints nothing for that layer).

In `recursive-on-child` the two patterns fill their rects with each other. Only the reference that
revisits a pattern already being rendered is circular, so `patt1`'s rect is filled with `patt2`'s
tile, whose rect keeps its stroke and drops only its fill. The tile therefore holds two layers of
the stroke in both rects, as Firefox 153 draws it (see the table below). Donner's seam columns
read 96, 255, 255 and 96, and Firefox reads 102 to 114. Both layers contribute fully;
the difference is where the two coincident half-strokes are composited:

- Tiles rasterize at twice the device scale of their parent surface: the outer tile at 5 raster
  pixels per unit, the nested tile at 10. Each layer's half stroke (39.5 to 40 units) covers half
  of outer texel 197 (39.4 to 39.6 units).
- Source-over treats the two half-covers as independent: 0.5 + 0.5 x 0.5 = 0.75 for texel 197.
  Device column 98 averages texels 196 (0) and 197 (0.75), giving 0.375, alpha 96. The
  horizontal seam gives 0.875 the same way, alpha 224.
- Measured: either layer alone reads 64 on column 98 (and 192 on the horizontal seam), on the
  software renderer and on Geode.
- Browsers composite the layers at device resolution instead: 1 - (1 - 0.25)^2 = 0.44, alpha 112.
  The true coverage of two coincident strokes is 0.25, so neither is exact; each counts the
  overlap at a different resolution.

The seam numbers below are for the self-recursive tiles, the single-layer case. At 2.5 device pixels
per unit, the vertical seam at x = 40 units covers device x 98.75 to 101.25. The exact coverage of
columns 98 to 101 is therefore 0.25, 1, 1 and 0.25, which is alpha 64, 255, 255 and 64. Donner draws
exactly that.

Donner does not draw pattern tiles analytically. Both backends rasterize the tile at twice its
device scale (`ComputePatternTileRasterMetrics`) and sample it bilinearly with repeat wrapping.
These rows come out exact because of their alignment. At 2.5 device pixels per unit, the
40 x 25 unit tile is exactly 200 x 125 raster pixels, and every stroke edge sits on a multiple of
half a raster pixel (98.75 device = 197.5 raster). Each device pixel center then falls on a
raster pixel boundary, so bilinear sampling averages exactly two raster pixels per axis. That
2x box filter reproduces the exact coverage of edges at those positions. Content whose edges fall
elsewhere in the tile is not reproduced exactly; `paint-servers/pattern/text-child` is reported
to read 57 on a glyph edge whose exact coverage is 64.

The vendored PNGs read 89, 242, 242 and 90, the profile of a rasterized tile resampled with a
filter. At the default threshold the rows differ from those PNGs by 4,053 (recursive-on-child) and
5,503 pixels, and by 0 pixels under a 0.2 color threshold.

`results.csv` records Chrome, Firefox and resvg passing all three vendored PNGs (Safari fails).
That is a visual verdict, and it cannot separate the two profiles: they differ by 25 of 255
levels on the two partial columns and 13 on the full ones, and both cover the seam with 2.5 to
2.6 pixels of stroke, a 4% difference in width at 2.5 pixels per unit.

What the browsers draw, measured on Chromium 147 and Firefox 153 at 500x500 (stroke coverage per
pixel, 0-255):

| Rendering | Vertical seam, x = 40 units (pixel-aligned), cols 98-101 | Horizontal seam, y = 25 units (fractional), rows 60-64 |
| --- | --- | --- |
| Donner | 64, 255, 255, 64 | 0, 192, 255, 192, 0 |
| Vendored PNG | 89, 242, 242, 90 | 12, 165, 255, 166, 13 |
| Chromium 147, self-recursive | 63, 255, 255, 64 | 32, 159, 255, 159, 32 |
| Firefox 153, self-recursive | 59, 255, 255, 64 | 28, 148, 255, 160, 33 |
| Vendored PNG, recursive-on-child lower rect (`rect4`) | 134, 248, 248, 135 | 18, 210, 255, 210, 19 |
| Chromium 147, recursive-on-child upper rect (`rect3`) | 110, 255, 255, 112 | 56, 184, 255, 184, 56 |
| Chromium 151, recursive-on-child lower rect (`rect4`) | 63, 255, 255, 64 | 32, 159, 255, 159, 32 |
| Firefox 153, recursive-on-child, both rects | 102, 255, 255, 114 | 49, 175, 255, 184, 57 |

The vendored PNG row above it is the self-recursive rows and the upper rect of
recursive-on-child.

Both browsers draw the exact coverage where the tile edge falls on a pixel boundary. Where it falls
mid-pixel (y = 25 units is device y 62.5), they draw a resampled tile, as the vendored PNG does
everywhere. Keeping the exact seams is a maintainer decision.

On `recursive-on-child`, `rect3` is filled with `patt1` and `rect4` with `patt2`. The second
stroke layer described above is drawn by Firefox 153 in both rects, by Chromium (147 and 151) in
`rect3` only, by the vendored PNG in `rect4` only, and by Donner in both rects: 96, 255, 255, 96
on the vertical seam and 0, 224, 255, 224, 0 on the horizontal one, in each rect. Donner
therefore draws the layer in the same rects as Firefox, and its partial columns read 96 where
Firefox reads 102 to 114, for the compositing reason above. SVG 2 does not say which reference of
a two-pattern cycle is the circular one; Donner treats only the reference that revisits a pattern
already being rendered as circular.

resvg v0.49.0's 300x300 references are no closer (3,282 to 3,617 pixels). At 1.5 device pixels
per unit, the horizontal seam at y = 50 units (device y 75) covers device y 74.25 to 75.75, which
is alpha 192 on rows 74 and 75. Those references read 63, 255 and 192 over rows 74 to 76: about
2.0 pixels of coverage where the exact stroke is 1.5, so the stroke is widened and offset. Donner
reads 192, 192 and 0.

PNG SHA-256:
- `donner-pattern-recursive-on-child.png`: `52f05db1cdf008bd95245a6c635bf7101858f885d200f45193905332721c2d65`
- `donner-pattern-self-recursive.png`: `0095bce42a5f9fde3bd33886679e07ba7d247a8265087e82fc8aed9175d7bb17`

To regenerate, delete the files and run from the repository root:

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_default_text -- '--gtest_filter=*PaintServersPattern*recursive*'
```

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

## Geode feGaussianBlur complex-transform golden

`geode/filters_feGaussianBlur_complex-transform.png` is the Geode rendering of the unchanged corpus
file `filters/feGaussianBlur/complex-transform` at 500x500, written by the resvg suite harness on
software Vulkan (Mesa 25.2.6 lavapipe, llvmpipe with LLVM 21.1.7) at the commit that adds it. Only
the `GeodeGolden` comparison uses it; the CPU renderer compares against the vendored reference and
differs from it by 99 pixels.

The rotated rect's covered area agrees with a 16x scan control to under 0.3 px in total. Coverage
along its edges differs from the reference's, and the directional `stdDeviation="12 0"` blur widens
that into a thin band, so the Geode render differs from the vendored reference by 247 pixels on
software Vulkan and 262 on Metal at the default threshold. Against this golden, at the default 0.02
threshold and 100-pixel allowance, software Vulkan differs by 0 pixels and Metal by 9.

The rotated filter runs in a local raster scaled by 2.5, so the filter region's edges fall on half
pixels there. The image keeps only the output pixels whose centers lie inside the filter region, the
rule both renderers apply, and so has no half-covered column of blurred content beyond the region.
Compared with the image it replaces, it differs in a three-pixel band along that far edge (427
pixels, 6 to 57 premultiplied levels), in a one-pixel line of 144 pixels along the rect's left edge
under its stroke (139 of them by 5 levels, the rest by 4 to 22), and elsewhere by at most 2 levels,
the low-order differences of the rendering environment.

To regenerate, delete the file and run from the repository root on Linux with the lavapipe ICD:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run --config=geode \
  //donner/svg/renderer/tests:resvg_test_suite_geode -- \
  '--gtest_filter=FiltersFeGaussianBlur/*complex_transform_GeodeGolden'
```

PNG SHA-256: `871da8f754631be06f0c5dd6d81ac07b4ca10287845f67ecc29f3796a131a718`.

## Goldens without recorded provenance

The renderer and the regeneration command of this golden are not recorded.

- `resvg-indirect.png` (`text/text-decoration/indirect`): provenance not recorded. It was first
  committed in `0c5402b1d` under a numbered corpus name, with a source comment saying the vendored
  PNG has a phantom green stroke, and kept its bytes through the rename in `4aef2b23c`. The
  vendored `indirect.png` outlines the glyphs and the underline in green, while the SVG paints
  no green: 2,269 of its pixels are exactly opaque RGBA (0, 128, 0, 255), and 7,309 have any
  alpha with a green channel above both red and blue. This golden has none by either count.

## Registered golden digests

Every golden that `donner/svg/renderer/tests/resvg_test_suite.cc` compares against is listed here
with the SHA-256 of the committed file and the section that records its provenance.
`//tools/resvg_parity:parity_report_tests` fails when a registered golden has no row in this table
(a row in another section does not count), or when any table row in this README that names it
carries a different digest. Update the row in the same change that
regenerates or adds a golden.

| Golden | SHA-256 | Provenance |
| --- | --- | --- |
| `donner-circle-shorthand-with-stroke-box.png` | `5bddc6ef292d0970c3571dead6e4d6a7b9588329d86183c633a7632f9915c3a3` | Donner renderings of disputed rows |
| `donner-circle-shorthand-with-view-box.png` | `bc60330d7e98ea0a01d40667ba2802d78b9a89e297d555431cffb11ac2fef1c3` | Donner renderings of disputed rows |
| `donner-color-adjust-functions-0percent.png` | `19d961480ad25de50d2e253e7a63651161165ccb34c85dc8094e1eef6e1829b3` | Color-adjust filter references |
| `donner-color-adjust-functions-100percent.png` | `f8a411299bd564da61e553a8d64b5f6fec46e80b807211b9ba526e42220823b6` | Color-adjust filter references |
| `donner-color-adjust-functions-200percent.png` | `e2a347ec867dba320fff3ec1b5eb375c2a83fcf9e38a0be6357533ec2a95fdef` | Color-adjust filter references |
| `donner-color-adjust-functions-50percent.png` | `ecd812076497b2d410aa9bb5c051f219311e5f9dce436cb0684b419ece8e48ea` | Color-adjust filter references |
| `donner-color-adjust-functions-negative.png` | `a971169dca1c19942291233b6ed4973346a7065bbc551f756ca5e0132f6b5e40` | Color-adjust filter references |
| `donner-complex-order-and-xlink-href.png` | `9962099aac41e77fd1d9044fd05d2c939dad0ecfdc941f98dc1b36dae9ab3750` | `filters/filter/complex-order-and-xlink-href` |
| `donner-coordinates-list.png` | `f036660ed5d2f21cbbedbbba1f04c8aa94f31cc33958a3dd63e5cf13bb52f5bb` | `text/text-anchor/coordinates-list` |
| `donner-drop-shadow-function-em-values.png` | `3bc724817809d4ed92c53c1b655a45f9f3a5a2a031efd4f0f99e48100abd9020` | `filters/filter-functions/drop-shadow-function*` |
| `donner-drop-shadow-function-mm-values.png` | `044fe3cb0fe0176ab7dcbe86831dd49998e68e55d0b77143c2a4ce5a4b04aa3f` | `filters/filter-functions/drop-shadow-function*` |
| `donner-drop-shadow-function.png` | `b5f628d66da78e36f887fedd444eeb9bcd0111b0d62a01ba79ceed9f53405c3c` | `filters/filter-functions/drop-shadow-function*` |
| `donner-feColorMatrix-identity.png` | `da52a1059a66c0ae3155dd5b3b5d5d155e6f6d4b88fab102e538f4183bc04916` | `filters/feColorMatrix` identity rows |
| `donner-font-simple-case.png` | `f4a40c7b0fe7dbc476eda3ce0fff92a35a00a38249b14dafa22625ba1120473c` | Donner renderings of disputed rows |
| `donner-invalid-FuncIRI.png` | `e1409e0b459dfb962a6c6a38ffe82fdaa57a4435a016696fb85ff0555558a50f` | Donner renderings of disputed rows |
| `donner-named-value-without-a-parent.png` | `65f5aa8f898bdb0f1586dd47b3db48ab830af39fe0cee350a8b01ac27e499e16` | Donner renderings of `text/font-size/named-value` and `named-value-without-a-parent` |
| `donner-named-value.png` | `52f758743a31018c94d2b16df37bdd4e8cf5c1b59237358a18c5f8e5f6234c12` | Donner renderings of `text/font-size/named-value` and `named-value-without-a-parent` |
| `donner-no-size.png` | `d999792b03ca8bb92b6bf440e030ea9f2b1ff803c0bd1567e34ebf45844c9d2a` | Donner renderings of disputed rows |
| `donner-one-invalid-url-in-list.png` | `383d16d1523001752ac353ce8875e1b900a1efa3561eb95fab9fb1d7a0ad598a` | Donner renderings of disputed rows |
| `donner-pattern-recursive-on-child.png` | `52f05db1cdf008bd95245a6c635bf7101858f885d200f45193905332721c2d65` | Donner renderings of recursive pattern references |
| `donner-pattern-self-recursive.png` | `0095bce42a5f9fde3bd33886679e07ba7d247a8265087e82fc8aed9175d7bb17` | Donner renderings of recursive pattern references |
| `donner-textPath-with-path.png` | `8e17141e31f90ea34371c432c95e7aa5870d7e068c76eece8c8a2190fa85f2d7` | `text/textPath/with-path` and `with-path-and-xlink-href` |
| `donner-textPath-writing-mode-tb.png` | `53f79bd0f51dea2d8499227ac55958e2e1bf3c32f5f124ad50018d4d95e88b1c` | Donner renderings of disputed rows |
| `donner-url-and-grayscale.png` | `934860bcb56d1f714c9aa5137ff13c52280258c9b9c5bf91ced54ed974db60b6` | Donner renderings of disputed rows |
| `donner-xlink-to-an-external-file.png` | `400dc754087e84efd15a428ad9671bcc34f808eccd147cb59231bd4bcba41565` | Donner renderings of disputed rows |
| `firefox-double-subregion-blur.png` | `49abff60095a6ebdddfb20edb4a7d842dd7a8efba295c6cf0a625a8764d451d9` | Gaussian blur primitive subregions |
| `firefox-stroke-linear-gradient-on-text.png` | `24b2bc34651ec46cdee7e49450e832916b1f0666bbb5cd20e3b543f93f55fdd4` | Gradient strokes on text |
| `firefox-stroke-radial-gradient-on-text.png` | `40695b1c5ff590b5367bd645e62dedb9855d4a3a8a7c1bafed16f424fb73dfd1` | Gradient strokes on text |
| `geode/donner-color-adjust-functions-0percent.png` | `ac9f02f71d8c116e36cdb476bf5d0702c863691c6db223f558052e73ebadff41` | Color-adjust filter references |
| `geode/donner-color-adjust-functions-100percent.png` | `a0801618b69a89113657590737904c73f0ea2f1f8684294ec4906e6abeabbc0a` | Color-adjust filter references |
| `geode/donner-color-adjust-functions-200percent.png` | `7872a95c6ad4b81072f6c0dcbb3c68818add843a3762d9f15f234645293dd3cc` | Color-adjust filter references |
| `geode/donner-color-adjust-functions-50percent.png` | `d17790f2c6ee6d19c152cc42f932ca44740f6076eb34a49248fb21ac9fdfa3bc` | Color-adjust filter references |
| `geode/donner-color-adjust-functions-negative.png` | `21ac65ab93ac649542f122e75ea04b88db85a9abb11970da46729b14301014d8` | Color-adjust filter references |
| `geode/filters_feGaussianBlur_complex-transform.png` | `871da8f754631be06f0c5dd6d81ac07b4ca10287845f67ecc29f3796a131a718` | Geode feGaussianBlur complex-transform golden |
| `resvg-all-types-inline-bands.png` | `4a9f8b2def1a673e91396d9eb64ff2e71aaada40ad5cb616ae1e56d3132ca911` | Inline text decoration and malformed lists |
| `resvg-all-types-inline-no-decoration.png` | `2550df9e39e1e949b633b924c98995203e5bc5c134bfb9537cf8a9868e2082f7` | Inline text decoration and malformed lists |
| `resvg-complex-graphemes-and-coordinates-list.png` | `0c37edfeafdd94c8a9b8414f1c5794fee897b933a0dbd6a3e1ad8087bcba6b83` | Cyrillic combining mark position |
| `resvg-complex-transform.png` | `6621a9e1494d58d650380f1683ddf75ec790bcda3aab84ffd990cee73fd3343a` | `filters/feSpotLight/complex-transform` |
| `resvg-compound-emojis-and-coordinates-list.png` | `9fef8a7afaf51ff396e4a776da3ec65f72f1b3a5c3ed59e55bd4b10d5ca2061f` | Emoji positioning reference |
| `resvg-dy-with-tiny-coordinates.png` | `518e1ba79b2d93a906aecf1d109fdb3362794b36fc2b7ac4df28cc4b80fa26a4` | Text path reference images |
| `resvg-embedded-svg-with-text.png` | `0f0c328a6c2c99e3ff09b3e4e0c8173d0720737fcafee7c4dbfb8f316cff0bf8` | structure/image: embedded SVG with text |
| `resvg-filter-input-and-subregion.png` | `f999add4a549c79697a68a2995f7d59b3cc2f09b69f4dfba8ae44058b7f3296f` | Filter input clipping and a narrower primitive subregion |
| `resvg-font-size-adjust-simple-case.png` | `7493132a5ca79910e11d3ece3c025cab71a14360162feb6e1e550df1c2762abb` | Donner-rendered font-size-adjust reference |
| `resvg-half-box-blur.png` | `0832ea8ff38488e742fcabc57b5ab408fb6ae85f852a310cda57674e7192a130` | Gaussian blur primitive subregions |
| `resvg-hanging-on-vertical.png` | `7adf76966bf9ae515051db400b4f29bd5f61952c8c37acd2b2ff2db4715e398d` | Donner-rendered references |
| `resvg-indirect.png` | `d0650c15a05856bb424850f3340ee59d1592d58648a120f4922a51962cb0a963` | Provenance not recorded; see Goldens without recorded provenance |
| `resvg-japanese-with-tb.png` | `f750e889461dea59c46ceb3945aac85f06ff5baa312ad7fb5b9300dc5e291812` | Donner-rendered references |
| `resvg-method=stretch.png` | `ba6c92599b34f0327da5a06536fa4b2f36d8487b83cab4138caa6be03f0e7626` | Donner-rendered textPath/method=stretch reference |
| `resvg-mixed-children-1.png` | `2ea922a8be96b59e9ed1950eae3bfcda2368710c80a8a6d4a24eb5db6d92a054` | Donner-rendered textPath continuation references |
| `resvg-mixed-children-2.png` | `15bb577cfdb17641732110e9ba2c3da36ec24b1761ee972d2e5881676fbd61a0` | Donner-rendered textPath/mixed-children-2 reference |
| `resvg-mixed-languages-with-tb.png` | `b3cd1778a62622798def3373bd7fe7c4e2188f9ba48a85a2c91129fbeb88eda7` | Donner-rendered references |
| `resvg-nested-rectangles.png` | `ce5bdadc7ee47341ac56618873b538017f97ad32532f228f0915536590b715bf` | Explicit tile geometry for nested and skewed patterns |
| `resvg-on-feImage.png` | `e94b02b863268c3579d36929691e9a8cfb5c389976fe459f00d2f3aa122a4154` | Nearest-neighbor sampling for `on-feImage` |
| `resvg-skewed-text-tiles.png` | `11804d5374ecb1b2777cae2540d0493764f47b9117e4bd9e837bb65651f6f477` | Explicit tile geometry for nested and skewed patterns |
| `resvg-tb-and-punctuation.png` | `808ecdad9093a3ae53788d51c85da384f842eee74ba6bf439c80e035e4caf6b7` | Donner-rendered references |
| `resvg-tspan-with-absolute-position.png` | `754eba7cfe125e9ae873187015247f89d7e9f5e8614f604a0ef7880c8c03eaa7` | Legacy PNGs (renderer and generation command not recorded) |
| `resvg-tspan-with-relative-position.png` | `fda4f9409e5b1cd52b3368157eca7ff5af20b438c68eb82b0e263b0cb33b8e83` | Text path reference images |
| `resvg-very-long-text.png` | `6392266f206470a9459514291d4b7b811f3a0b9d406c7fc2d81fdb3ded914021` | Donner-rendered textPath continuation references |
| `resvg-with-underline.png` | `3ff550f2200e10a348a24a74aef99196e867fbd4a9249c7c109ea85aca982af2` | Legacy PNGs (renderer and generation command not recorded) |
| `resvg-xlink-filter-bounds.png` | `9ebb7922a3da396ec9a892db7850c929acf2f1af071c7a0db672e38be1ad18e9` | Filter input clipping and a narrower primitive subregion |
