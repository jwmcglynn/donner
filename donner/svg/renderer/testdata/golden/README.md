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

## Donner renderings of text references with an opaque background

The vendored PNGs for the rows below are opaque white, while every other vendored reference
and Donner's canvas are transparent. Under the comparator's checkerboard blending that
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
