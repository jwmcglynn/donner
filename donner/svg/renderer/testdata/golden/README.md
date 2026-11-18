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

## Donner renderings of `text/font-size/named-value` and `named-value-without-a-parent`

`donner-named-value.png` and `donner-named-value-without-a-parent.png` are 500x500 renderings by
the software renderer with simple text at commit `21c59c75`, at default settings. Full text
differs from `donner-named-value.png` by 13 pixels and from the other by 0, on macOS and on
Linux aarch64; Geode does not render these text rows.

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
| `donner-named-value.png` | `7e65b966683f6daad8a6b931d67bc8a61be0de4a29f606071b3967cfa22fd3cf` |
| `donner-named-value-without-a-parent.png` | `65f5aa8f898bdb0f1586dd47b3db48ab830af39fe0cee350a8b01ac27e499e16` |

To regenerate, delete a file and run from the repository root:

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_default_text -- '--gtest_filter=*TextFontSize*named_value*'
```

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

## Upright Latin outline reference

`text/glyph-orientation-vertical/simple-case.svg` uses the SVG 2 alias
`glyph-orientation-vertical="0"` for upright text. Donner implements that alias. Google Chrome
158.0.8127.37 and Firefox 157 ignore it, so the browser control used `text-orientation: upright`
with the same vendored Noto Sans face at 32 user units. Chrome's glyph centroids were within a
device pixel of the historical PNG. Firefox used the same 43.60-user-unit advance and a higher
synthesized origin. CSS Writing Modes leaves the origin to the user agent for faces without
vertical metrics.

The Noto Sans face has no `vmtx` or `VORG`. The control starts at y=30 and adds the scaled
`hhea` ascent (1,069/1,000 em). Each letter advances by the `OS/2` typographic height (1,362/1,000
em), giving four baselines at 64.208, 107.792, 151.376, and 194.960. Each letter is a separate
horizontal text element with `x="100"`, `text-anchor="middle"`, and kerning disabled.
usvg 0.49.0 converts those four letters to outlines using the corpus font. The expected SVG
substitutes only those outlines. The crosshairs and frame are the original vector elements.

At 500 × 500 the updated renderer differs from the historical PNG by 733 pixels in each CPU
tier. Of these, 731 are on the crosshairs. At x=120 the PNG's stroke covers three rows with alpha
32, 255, 32, while the current render covers two with alpha 128, 192. Stable Chrome also covers
two rows. The outlined reference gives zero differences on both tiers at the default 0.02
threshold and 100-pixel limit. It tests the full scene, including the strokes.

| Input or output | SHA-256 |
| --- | --- |
| Corpus SVG | `fdfb95427efee7325fcef8f2a19949486bb1ba12b3b8adacc34965d508451240` |
| Corpus PNG | `37a4b1991f55b498519f2b8dc06ea995d2f3bb5541b3c01b728a0a45a7b8f240` |
| corpus Noto Sans Regular | `6b04c8dd65af6b73eb4279472ed1580b29102d6496a377340e80a40cdb3b22c9` |
| `donner-upright-latin-outline.svg` | `8fb7c58c23e1c32899dbe610171d9d91a23106fbaf41e0ba0c3071d3d6c38693` |
| `donner-upright-latin-outline.png` | `33bd0044926d60bbf4bb03dd2423d599e9141e6c62a9d4ebb436e2a3d072e766` |

The checked-in outline SVG is the input for PNG regeneration:

```sh
bazel run //donner/svg/tool:donner-cli -- \
  donner/svg/renderer/testdata/golden/donner-upright-latin-outline.svg \
  --width 500 --height 500 --output donner/svg/renderer/testdata/golden/donner-upright-latin-outline.png
```

## Donner-rendered references

The references below are Donner renderings. Each replaces a vendored reference that encodes a
renderer's own model where browsers measurably agree with Donner instead; a self-rendered golden
guards against regressions but does not prove correctness, so each entry names the independent
evidence. Each was rendered by the full text tier with the basic (TinySkia) backend from the code of
the change that added it, regenerates byte for byte from that code with the command below, and
compares at default parameters.

To regenerate one, delete its PNG and run from the repository root (the fixture writes a missing
golden):

```sh
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run \
  //donner/svg/renderer/tests:resvg_test_suite_max -- '--gtest_filter=*<case_with_underscores>*'
```

| Case | Why the vendored reference is not used | SVG SHA-256 | PNG SHA-256 |
| ---- | -------------------------------------- | ----------- | ----------- |
| `text/writing-mode/japanese-with-tb` | Upright U+65E5 and U+672C in Mplus 1p at 64 user units. Blink (OpenTypeVerticalData::GetVerticalTranslationsForGlyphs) and Gecko (gfxHarfBuzzShaper::GetGlyphVOrigin) take the upright vertical origin from VORG, then from the vmtx top side bearing over the glyph top, and only then synthesize one, and CSS Writing Modes 3 section 5.1.1 requires the font's vertical metrics. Mplus 1p has vhea and vmtx, with a vertical origin of 860 units for every glyph here; resvg ignores vmtx and centers the glyph on the hhea ascent and descent, as the vendored PNG does. Against the vendored PNG Donner's two glyphs sit 5.63 and 5.58 device px higher (5801 px differ); Chrome 156 with the font applied places them 5.83 and 5.66 px higher, within 0.25 px of Donner, so the browser measurement clearly favors Donner. Caveat: the corpus file writes font-family="Mplus 1p" unquoted, which is invalid CSS because of the 1p token, so browsers fall back to a default font, and the corpus results' browser passes for this row reflect that fallback; the Chrome measurement used a copy that differs only by quoting the family name. | `a7cc22f6a69b9cf4c4025372bbd0bf167f6d9a31c31e515db0375f30521f020f` | `f750e889461dea59c46ceb3945aac85f06ff5baa312ad7fb5b9300dc5e291812` |
| `text/writing-mode/mixed-languages-with-tb` | Upright U+65E5 and U+672C in Mplus 1p, sideways Latin and Arabic, at 20 user units on the row's 300x300 canvas. CSS Writing Modes 3 section 5.1.1 requires the font's vertical metrics for upright glyphs; Mplus 1p has vhea and vmtx, with a vertical origin of 860 units, which Blink and Gecko take from VORG and then from vmtx, as Donner does. resvg centers upright glyphs on the hhea ascent and descent instead, and the vendored PNG sets both upright glyphs 0.35 to 0.5 device px lower than Donner. Horizontal stroke centers in device px, top to bottom, measured from renders with the corpus fonts loaded through @font-face in headless Chromium 151 and Firefox 153: U+65E5 at 45.43, 55.75 and 66.75 in Donner, 45.49, 55.58 and 66.68 in Firefox, 46.50, 56.50 and 67.74 in Chromium (on whole pixel rows) and 45.79, 56.25 and 67.25 in the vendored PNG; U+672C at 79.00 and 93.39 in Donner, 79.09 and 93.46 in Firefox, 79.09 and 93.45 in Chromium and 79.39 and 93.74 in the vendored PNG. The sideways Latin and Arabic match the vendored PNG within 0.15 px, and all 108 px that differ from it lie on the two upright glyphs. The corpus results mark Chrome, Firefox and resvg as passing and Safari as failing this row. | `3bacf63bdeb8b168a8ca061984e529b6a12d03f8cfa8c163fdbf8a1e92265544` | `b3cd1778a62622798def3373bd7fe7c4e2188f9ba48a85a2c91129fbeb88eda7` |
| `text/writing-mode/tb-and-punctuation` | Mplus 1p kana, ideographs and vertical punctuation forms at 16 user units. Blink (OpenTypeVerticalData::GetVerticalTranslationsForGlyphs) and Gecko (gfxHarfBuzzShaper::GetGlyphVOrigin) take the upright vertical origin from VORG, then from the vmtx top side bearing over the glyph top, and only then synthesize one, and CSS Writing Modes 3 section 5.1.1 requires the font's vertical metrics. Mplus 1p has vhea and vmtx, with a vertical origin of 860 units for every glyph here; resvg ignores vmtx and centers the glyph on the hhea ascent and descent, as the vendored PNG does. Donner and Chrome 156 both place the vertical comma and full stop forms (U+3001, U+3002) about 14 device px right of the vendored PNG, in the upper right of their cells, and agree within 0.4 px on all 12 ink clusters, which sit up to 1.65 px above the PNG; the punctuation forms clearly favor Donner. 228 px differ. The corpus results mark Chrome and Firefox as passing and Safari and resvg as failing this row. | `100f4e24b2fd7013e554303f71b9b63199789863e81e86f8b046f340c6e34028` | `808ecdad9093a3ae53788d51c85da384f842eee74ba6bf439c80e035e4caf6b7` |
| `text/alignment-baseline/hanging-on-vertical` | alignment-baseline: hanging in vertical text. resvg does not apply alignment-baseline in vertical layout, so the vendored PNG shows the default central baseline and wider spacing. Donner and Chrome 156 both apply the hanging baseline and agree within 1.1 device px on all 7 ink clusters, which sit 1.1 to 46 px from the PNG along the column when matched in reading order (1.1 and 21.9 px for the two upright glyphs, 44 to 46 px for the five sideways letters), so the browser measurement clearly favors Donner. The upright U+65E5 and U+672C sit at Mplus 1p's vmtx origin; the sideways "Japan" is rotated onto the hanging baseline and has no vertical origin of its own. The corpus results mark Chrome and Safari as passing and Firefox and resvg as failing. The simple text tier differs from this full-tier rendering by 10 px. | `cb9c38a6b0542998f1be4ff2efbc14cd39eb656f4e29547542d65bebca6378d6` | `7adf76966bf9ae515051db400b4f29bd5f61952c8c37acd2b2ff2db4715e398d` |

## Mixed vertical underline outline reference

`text/writing-mode/mixed-languages-with-tb-and-underline.svg` uses Amiri for Latin and joined
Arabic, and Mplus 1p for the upright ideograph. The historical PNG differs by 1,331 pixels,
including 929 at the underline. CSS Text Decoration leaves the underline position and
thickness to the user agent under `auto`. Firefox 157 covers columns 210–211 on the 500 × 500
canvas. Google Chrome 158.0.8127.37 covers 219–223 with either its fallback or the corpus Mplus
face. The historical mask covers 204–207.

The control derives the column from design-unit advances. At a font size of 20, HarfBuzz shapes
the sideways Latin segment with its two spaces to 35.87890625 units, and the joined Arabic segment
to 37.197265625. The CJK cell advances 20 units and takes the Mplus `vmtx` origin of 860/1,000 em.
Centering the total 93.076171875-unit advance at y=100 puts the column start at 53.4619140625.
The sideways runs use the alphabetic baseline at x=95.1171875 from Amiri's `hhea` ascent and
descent (2,300 and -1,300 at 2,048 units per em). The Latin outlines start after the 600-design-unit
leading space. The Arabic is shaped as one horizontal right-to-left word and rotated clockwise
onto the same baseline.

Amiri's `post` offset is -1,151 units and its thickness is 102 units. These give the underline
rectangle x=83.37890625 and width 0.99609375 in the reference SVG. Its three device-pixel
columns have alpha 141, 255, and 239. Both the underline and the outlines are vector geometry.
usvg 0.49.0 supplies the glyph outlines from the isolated horizontal text runs, and the
crosshairs and frame use the original SVG vectors. The full-text tier matches the entire
control with zero differences at the default threshold and limit.

| Input or output | SHA-256 |
| --- | --- |
| Corpus SVG | `81570913183763bb689de8434401842ededea3b6506d6ee6d11b181cb77b91f5` |
| Corpus PNG | `2fa7d4e007f88ff94e8c7319559032b3c3d5f0a163c0a8dfee3fbc270491c01e` |
| Amiri Regular | `f3615b08cc3d78409474c28ed7c9e0fb5cc42435e12080a385e1a4163ab46708` |
| Mplus 1p Regular | `c6a5f15dcbce870497c580de7fa578a75eb8846d2b5603a65eb8fb69ab949892` |
| `donner-mixed-vertical-underline-outline.svg` | `6f5a23b713f46bd9f2f3829cc21c63b5c225b0d281035dbca84bee514e0c3596` |
| `donner-mixed-vertical-underline-outline.png` | `129d7b96a7fcf32aef0a41ff74fd5dd8c62033fe9650bd4423bf96fc328db238` |

```sh
bazel run //donner/svg/tool:donner-cli -- \
  donner/svg/renderer/testdata/golden/donner-mixed-vertical-underline-outline.svg \
  --width 500 --height 500 \
  --output donner/svg/renderer/testdata/golden/donner-mixed-vertical-underline-outline.png
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
