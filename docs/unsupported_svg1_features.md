# Unsupported SVG 1.1 Features

Donner targets **SVG 2** conformance. The following SVG 1.1 features are deprecated or removed
in SVG 2 and are intentionally not implemented.

## Filter Inputs: `BackgroundImage` / `BackgroundAlpha`

**SVG 1.1 behavior:** The `enable-background` attribute on a container element captured a snapshot
of the canvas behind it. Filter primitives could reference this snapshot via `in="BackgroundImage"`
or `in="BackgroundAlpha"`.

**SVG 2 status:** Removed. The SVG 2 spec no longer defines `enable-background`,
`BackgroundImage`, or `BackgroundAlpha`.

**Donner behavior:** These input names are parsed as unresolved named references, producing
transparent black (the SVG spec behavior for unresolved filter inputs). No warning is emitted.

**Modern alternative:** CSS compositing via `mix-blend-mode` and `isolation` properties provides
equivalent functionality for blending with backdrop content.

**Affected resvg tests:** `a-enable-background-*` (category disabled), `e-filter-032` (skipped),
`e-filter-033` (skipped).

## SVG Fonts (`<font>`, `<glyph>`, `<missing-glyph>`, etc.)

**SVG 1.1 behavior:** Defined an inline font format using SVG path data for glyph outlines.

**SVG 2 status:** Removed entirely. WOFF/WOFF2 and system fonts are the standard mechanisms.

**Donner behavior:** These elements are not recognized. Donner supports TrueType/OpenType fonts
via stb_truetype, WOFF2 via Brotli decompression, and optionally HarfBuzz for complex text shaping.

## `<cursor>` Element

**SVG 1.1 behavior:** Defined a custom cursor image inline in SVG.

**SVG 2 status:** Removed. CSS `cursor` property is the standard mechanism.

**Donner behavior:** Not implemented.

## `<altGlyph>`, `<altGlyphDef>`, `<altGlyphItem>`, `<glyphRef>`

**SVG 1.1 behavior:** Allowed substituting alternate glyph representations.

**SVG 2 status:** Removed. OpenType font features (accessed via `font-feature-settings`) provide
this functionality.

**Donner behavior:** Not implemented.

## `<tref>` Element

**SVG 1.1 behavior:** Referenced text content from another element by ID.

**SVG 2 status:** Removed.

**Donner behavior:** Not implemented.

## `clip` Property (CSS 2 clip rect)

**SVG 1.1 behavior:** `clip: rect(top, right, bottom, left)` on `<svg>`, `<symbol>`, `<image>`,
`<foreignObject>`, `<pattern>`.

**SVG 2 status:** Deprecated in favor of `clip-path`.

**Donner behavior:** Not implemented. Use `clip-path` with `clipPathUnits="userSpaceOnUse"` and
an `inset()` shape instead.

## `xml:base`, `xml:lang`, `xml:space`

**SVG 2 status:** `xml:base` removed. `xml:lang` replaced by `lang`. `xml:space` replaced by
CSS `white-space`.

**Donner behavior:** `xml:space` is parsed for `<text>` whitespace handling. `xml:base` and
`xml:lang` are not implemented.
