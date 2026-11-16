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

## Donner renderings of recursive pattern references

`paint-servers/pattern/recursive-on-child` compares against `donner-pattern-recursive-on-child.png`.
`self-recursive` and `self-recursive-on-child` render identically and share
`donner-pattern-self-recursive.png`. All three are 500x500 renderings by the software renderer
at default settings: the shared self-recursive golden at commit `21c59c75`, and the
recursive-on-child golden with the active-chain recursion fix on this branch. Full text and Geode
on Linux lavapipe (aarch64) render all three identically (0 pixels counted); Geode on Metal
renders the self-recursive rows identically.

Each tile is a 40 x 25 unit rect whose 1-unit stroke straddles the tile edge, so each tile
clips half of its stroke and two neighboring tiles draw the two halves. In the self-recursive rows
the rect's fill references its own pattern, a circular reference, so that fill layer is not
painted (SVG 2 Linking, "circular references"; Painting, "Specifying paint": an invalid paint
server reference without a fallback paints nothing for that layer).

In `recursive-on-child` the two patterns fill their rects with each other. Only the reference that
revisits a pattern already being rendered is circular, so `patt1`'s rect is filled with `patt2`'s
tile, whose rect keeps its stroke and drops only its fill. The tile therefore holds two layers of
the stroke, which is what Chromium 147 and Firefox 153 draw (see the table below). Donner's seam
columns read 96, 255, 255 and 96 there, where the first-layer-only rendering read 64, 255, 255
and 64. Two layers of 0.25 coverage composite to 1 - (1 - 0.25)^2 = 0.44 (alpha 112), and the
browsers read 102 to 114. Donner's inner layer contributes about 0.17 instead of 0.25 on those
columns; the nested tile passes through a second raster resampling, which has not yet been
confirmed as the cause.

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
| Chromium 147, recursive-on-child | 110, 255, 255, 112 | 56, 184, 255, 184, 56 |
| Firefox 153, recursive-on-child | 102, 255, 255, 114 | 49, 175, 255, 184, 57 |

Both browsers draw the exact coverage where the tile edge falls on a pixel boundary. Where it falls
mid-pixel (y = 25 units is device y 62.5), they draw a resampled tile, as the vendored PNG does
everywhere. Keeping the exact seams is a maintainer decision. On `recursive-on-child` both browsers
and Donner paint the second stroke layer described above (Donner: 96, 255, 255, 96 on the
vertical seam and 0, 224, 255, 224, 0 on the horizontal one); the vendored PNG draws one layer.

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
