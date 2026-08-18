# Unsupported SVG 1.1 Features {#UnsupportedSvg1Features}

Donner targets **SVG 2** conformance. The following SVG 1.1 features are deprecated or removed
in SVG 2 and are intentionally not implemented.

## Filter Inputs: BackgroundImage and BackgroundAlpha

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

## SVG Fonts (&lt;font&gt;, &lt;glyph&gt;, &lt;missing-glyph&gt;, etc.)

**SVG 1.1 behavior:** Defined an inline font format using SVG path data for glyph outlines.

**SVG 2 status:** Removed entirely. WOFF/WOFF2 and system fonts are the standard mechanisms.

**Donner behavior:** These elements are not recognized. Donner supports TrueType/OpenType fonts
via stb_truetype, WOFF2 via Brotli decompression, and optionally HarfBuzz for complex text shaping.

## &lt;cursor&gt; Element

**SVG 1.1 behavior:** Defined a custom cursor image inline in SVG.

**SVG 2 status:** Removed. CSS `cursor` property is the standard mechanism.

**Donner behavior:** Not implemented.

## &lt;altGlyph&gt;, &lt;altGlyphDef&gt;, &lt;altGlyphItem&gt;, &lt;glyphRef&gt;

**SVG 1.1 behavior:** Allowed substituting alternate glyph representations.

**SVG 2 status:** Removed. OpenType font features (accessed via `font-feature-settings`) provide
this functionality.

**Donner behavior:** Not implemented.

## &lt;tref&gt; Element

**SVG 1.1 behavior:** Referenced text content from another element by ID.

**SVG 2 status:** Removed.

**Donner behavior:** Not implemented.

## Legacy Glyph Orientation Properties

**SVG 1.1 behavior:** `glyph-orientation-horizontal` and `glyph-orientation-vertical` selected
per-glyph rotation in horizontal and vertical text.

**SVG 2 status:** `glyph-orientation-horizontal` was removed. `glyph-orientation-vertical` was
obsoleted and replaced by `text-orientation`; SVG 2 retains only a narrow compatibility alias for
`auto`, `0`, and `90` values.

**Donner behavior:** The legacy property names are recognized but do not have independent layout
behavior. The narrow `glyph-orientation-vertical` compatibility alias is not yet applied. Modern
vertical text should use `writing-mode: vertical-rl` or `vertical-lr` together with
`text-orientation`.

## Obsolete SVG 1.1 Writing-Mode Values

**SVG 1.1 behavior:** Values such as `lr`, `lr-tb`, `rl`, `rl-tb`, `tb`, `tb-rl`, and `tb-lr`
selected horizontal or vertical text flow.

**SVG 2 status:** The distinct values are obsolete. CSS Writing Modes maps them to
`horizontal-tb`, `vertical-rl`, or `vertical-lr` for compatibility.

**Donner behavior:** The parser immediately maps the obsolete spellings to their modern computed
values. There is no separate legacy layout path.

## The Clip Property (CSS 2 Clip Rect)

**SVG 1.1 behavior:** `clip: rect(top, right, bottom, left)` on `<svg>`, `<symbol>`, `<image>`,
`<foreignObject>`, `<pattern>`.

**SVG 2 status:** Deprecated in favor of `clip-path`.

**Donner behavior:** Not implemented. Use `clip-path` with `clipPathUnits="userSpaceOnUse"` and
an `inset()` shape instead.

## XML Language and Whitespace Attributes

**SVG 2 status:** `xml:base` was removed. `xml:lang` remains a current XML language attribute, and
SVG 2 also defines `lang`. `xml:space` was replaced by CSS `white-space`.

**Donner behavior:** `xml:space` is parsed for `<text>` whitespace handling. `xml:base` and
`xml:lang` are not implemented. The implemented `systemLanguage` conditional-processing attribute
is a separate feature and does not supply text language or font selection.
