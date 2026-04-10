# resvg-test-suite upgrade: migration report

- Total overrides in old file: **262**
- Successfully migrated:        **261**
- Orphaned (deleted upstream):  **1**
- Ambiguous (inline comments):  **2**
- Golden overrides renamed:     **35**

- Distinct new categories: **53**

## Per-category entry counts

- `filters/feColorMatrix`: 13 entries
- `filters/feConvolveMatrix`: 6 entries
- `filters/feDiffuseLighting`: 1 entries
- `filters/feDropShadow`: 2 entries
- `filters/feGaussianBlur`: 2 entries
- `filters/feImage`: 13 entries
- `filters/feMerge`: 1 entries
- `filters/fePointLight`: 1 entries
- `filters/feSpecularLighting`: 1 entries
- `filters/feSpotLight`: 1 entries
- `filters/feTile`: 1 entries
- `filters/feTurbulence`: 3 entries
- `filters/filter`: 12 entries
- `filters/flood-color`: 1 entries
- `masking/clip`: 1 entries
- `masking/clipPath`: 9 entries
- `masking/mask`: 4 entries
- `paint-servers/linearGradient`: 1 entries
- `paint-servers/pattern`: 11 entries
- `paint-servers/radialGradient`: 7 entries
- `paint-servers/stop`: 1 entries
- `painting/display`: 1 entries
- `painting/fill`: 6 entries
- `painting/marker`: 8 entries
- `painting/opacity`: 1 entries
- `painting/stroke`: 3 entries
- `painting/stroke-dasharray`: 3 entries
- `painting/stroke-linejoin`: 2 entries
- `painting/stroke-width`: 1 entries
- `painting/visibility`: 1 entries
- `shapes/line`: 1 entries
- `structure/defs`: 1 entries
- `structure/image`: 6 entries
- `structure/style`: 2 entries
- `structure/style-attribute`: 1 entries
- `structure/svg`: 24 entries
- `structure/symbol`: 1 entries
- `structure/transform`: 1 entries
- `structure/use`: 1 entries
- `text/baseline-shift`: 2 entries
- `text/font`: 1 entries
- `text/font-family`: 10 entries
- `text/font-size`: 3 entries
- `text/font-variant`: 2 entries
- `text/letter-spacing`: 4 entries
- `text/text`: 14 entries
- `text/text-anchor`: 2 entries
- `text/text-decoration`: 6 entries
- `text/textLength`: 1 entries
- `text/textPath`: 36 entries
- `text/tspan`: 9 entries
- `text/word-spacing`: 1 entries
- `text/writing-mode`: 14 entries

## Orphaned entries (require manual decision)

- `a-color-interpolation-filters-001.svg` — `Params::WithThreshold(0.05f)` // TinySkia linearRGB blur edge regression (was in Color)

## Ambiguous entries (inline comments in params)

- `e-text-030.svg` → `tests/text/text/x-and-y-with-multiple-values-and-arabic-text.svg`
  - params: `Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-x-and-y-with-multiple-values-and-arabic-text.png") .withMaxPixelsDifferent(400) // Vertical axis has different AA (its // not the focus of the test) .onlyTextFull()`
  - comment: Arabic text shaping requires text-full, and this
- `e-text-031.svg` → `tests/text/text/zalgo.svg`
  - params: `Params() .withMaxPixelsDifferent(300) // Vertical axis has different AA (its // not the focus of the test) .onlyTextFull()`
  - comment: Complex diatrics requires text-full

## Golden override renames

- `"donner/svg/renderer/testdata/golden/" "resvg-a-font-size-005.png"` → `donner/svg/renderer/testdata/golden/resvg-named-value.png`
- `"donner/svg/renderer/testdata/golden/" "resvg-a-font-size-008.png"` → `donner/svg/renderer/testdata/golden/resvg-named-value-without-a-parent.png`
- `"donner/svg/renderer/testdata/golden/resvg-a-text-decoration-019.png"` → `donner/svg/renderer/testdata/golden/resvg-indirect.png`
- `"donner/svg/renderer/testdata/golden/" "resvg-e-feImage-002.png"` → `donner/svg/renderer/testdata/golden/resvg-svg.png`
- `"donner/svg/renderer/testdata/golden/" "resvg-e-feSpecularLighting-002.png"` → `donner/svg/renderer/testdata/golden/resvg-with-fePointLight.png`
- `"donner/svg/renderer/testdata/golden/" "resvg-e-feSpotLight-012.png"` → `donner/svg/renderer/testdata/golden/resvg-complex-transform.png`
- `"donner/svg/renderer/testdata/golden/" "resvg-e-marker-019.png"` → `donner/svg/renderer/testdata/golden/resvg-with-an-image-child.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-marker-045.png"` → `donner/svg/renderer/testdata/golden/resvg-orient=auto-on-M-L-Z.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-marker-051.png"` → `donner/svg/renderer/testdata/golden/resvg-orient=auto-on-M-C-C-4.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-text-026.png"` → `donner/svg/renderer/testdata/golden/resvg-complex-graphemes-and-coordinates-list.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-text-029.png"` → `donner/svg/renderer/testdata/golden/resvg-compound-emojis-and-coordinates-list.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-text-030.png"` → `donner/svg/renderer/testdata/golden/resvg-x-and-y-with-multiple-values-and-arabic-text.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-text-036.png"` → `donner/svg/renderer/testdata/golden/resvg-rotate-on-Arabic.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-001.png"` → `donner/svg/renderer/testdata/golden/resvg-simple-case.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-002.png"` → `donner/svg/renderer/testdata/golden/resvg-startOffset=30.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-003.png"` → `donner/svg/renderer/testdata/golden/resvg-startOffset=5mm.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-004.png"` → `donner/svg/renderer/testdata/golden/resvg-startOffset=10percent.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-005.png"` → `donner/svg/renderer/testdata/golden/resvg-startOffset=-100.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-009.png"` → `donner/svg/renderer/testdata/golden/resvg-two-paths.png`
- `"donner/svg/renderer/testdata/golden/" "resvg-e-textPath-010.png"` → `donner/svg/renderer/testdata/golden/resvg-nested.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-011.png"` → `donner/svg/renderer/testdata/golden/resvg-mixed-children-1.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-013.png"` → `donner/svg/renderer/testdata/golden/resvg-with-coordinates-on-text.png`
- `"donner/svg/renderer/testdata/golden/" "resvg-e-textPath-014.png"` → `donner/svg/renderer/testdata/golden/resvg-with-coordinates-on-textPath.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-015.png"` → `donner/svg/renderer/testdata/golden/resvg-very-long-text.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-019.png"` → `donner/svg/renderer/testdata/golden/resvg-with-text-anchor.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-022.png"` → `donner/svg/renderer/testdata/golden/resvg-tspan-with-absolute-position.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-023.png"` → `donner/svg/renderer/testdata/golden/resvg-tspan-with-relative-position.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-026.png"` → `donner/svg/renderer/testdata/golden/resvg-path-with-ClosePath.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-027.png"` → `donner/svg/renderer/testdata/golden/resvg-m-L-Z-path.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-028.png"` → `donner/svg/renderer/testdata/golden/resvg-with-underline.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-029.png"` → `donner/svg/renderer/testdata/golden/resvg-with-rotate.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-032.png"` → `donner/svg/renderer/testdata/golden/resvg-with-baseline-shift.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-035.png"` → `donner/svg/renderer/testdata/golden/resvg-dy-with-tiny-coordinates.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-036.png"` → `donner/svg/renderer/testdata/golden/resvg-with-transform-on-a-referenced-path.png`
- `"donner/svg/renderer/testdata/golden/resvg-e-textPath-037.png"` → `donner/svg/renderer/testdata/golden/resvg-with-transform-outside-a-referenced-path.png`

