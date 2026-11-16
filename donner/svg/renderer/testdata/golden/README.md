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
