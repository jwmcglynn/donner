# Text Rendering: Testing and Validation

[Back to hub](../0010-text_rendering.md)

**Author:** Claude Opus 4.6

## Testing and Validation

### Unit tests

- `FontManager` tests: load TTF/OTF/WOFF1, verify `stbtt_fontinfo` initialization, test
  cascade fallback, verify font metrics.
- `TextEngine` helper tests: verify chunking, text-anchor, `textLength`, `lengthAdjust`,
  and baseline-shift calculations without real fonts.
- `TextBackend` tests: verify shaping, metrics, and outline extraction for both base and full
  backends.
- Glyph outline tests: extract outlines for known glyphs, verify path geometry matches expected
  curves.

### Golden image tests

- Add text-specific SVG test cases to `renderer_tests`:
  - Basic Latin text.
  - Multi-span positioning (dx/dy).
  - `text-anchor` (start/middle/end).
  - `@font-face` with embedded WOFF1 font.
  - `@font-face` with WOFF2 font (requires `text_full`).
  - Stroke + fill text.
  - Per-glyph rotation.
- Resvg text test subset: most `e-text-*` and `e-tspan-*` tests now pass with tight
  thresholds. Primary remaining failures are in `e-textPath-*` and `a-font-size-*`.

### Feature-gated test skipping

```cpp
{"text_basic.svg", Params::RequiresFeature(Feature::Text)},
{"text_woff2_font.svg", Params::RequiresFeature(Feature::TextFull)},
```

CI runs all feature combinations to ensure both enabled and disabled paths work.

### Backend parity

- `RendererTinySkia` and `RendererGeode` both consume the same `TextEngine`/`TextBackend*` layout
  output, so glyph positions should match between them at both the base and `text_full` tiers.
  Residual pixel differences still require root-cause analysis. See
  [0038-geode_tinyskia_text_parity.md](../0038-geode_tinyskia_text_parity.md) for the parity gate.
- The base tier (stb_truetype, `TextBackendSimple`) and `text_full` tier (HarfBuzz,
  `TextBackendFull`) can still diverge in glyph advances/kerning between each other, since only
  `text_full` processes GPOS.

## Current Snapshot {#current-snapshot}

Base-tier (`text`, TinySkia) resvg status for the most relevant text slices. This is a
living reference: regenerate the counts with
`bazel test //donner/svg/renderer/tests:resvg_test_suite_default_text` and read
`bazel-testlogs/.../resvg_test_suite_default_text/test.xml` (enabled = `status="run"`,
disabled = `status="notrun"`). The suite is green, so every enabled test passes —
some via blessed per-test goldens catalogued in
[0009](../0009-resvg_test_suite_bugs.md).

| Slice | Enabled passing | Disabled | Notes |
|---|---|---|---|
| `e-text-*` | 31/31 | 15 | Disabled are mostly text-full-only or explicit skips |
| `e-tspan-*` | 24/24 | 7 | |
| `a-text-decoration-*` | 19/19 | 2 | Includes a custom golden for the `indirect` case |
| `a-lengthAdjust-*` | 1/1 | 3 | Enabled coverage is very thin |
| `a-dominant-baseline-*` | 7/7 | 14 | Coverage is still thin |
| `a-letter-spacing-*` | 9/9 | 3 | Arabic case requires text-full; 3 disabled/UB cases remain |
| `a-textLength-*` | 7/7 | 5 | `a-textLength-008` still disabled |
| `a-writing-mode-*` | 13/13 | 10 | 10 disabled mixed-script / rotate / dx/dy cases remain |
| `e-textPath-*` | 33/33 | 11 | Enabled via blessed sub-pixel/curved-baseline goldens (see 0009) |

Release-significant gaps from the current snapshot:

- BiDi and advanced mixed-script vertical text remain explicitly deferred (the bulk of
  the disabled `a-writing-mode-*` / `a-dominant-baseline-*` cases).
- Several slices (`a-lengthAdjust-*`, `a-dominant-baseline-*`) have thin enabled
  coverage even though what is enabled passes.

## Known gaps and disabled tests {#known-gaps}

The suite is green: every enabled resvg text test passes (some via blessed per-test
goldens catalogued in [0009](../0009-resvg_test_suite_bugs.md)). The remaining work is
the set of tests still marked `Params::Skip(...)` / `DISABLED_` in
`donner/svg/renderer/tests/resvg_test_suite.cc`. That source — its per-test skip
reasons — is the authoritative list; the categories below summarize it.

### Unimplemented features

- **Bidirectional / RTL text (UAX#9)** — the largest gap. `direction` and `unicode-bidi`
  properties, BiDi reordering, and any span mixing LTR Latin with RTL Arabic are not
  implemented; several letter-spacing / writing-mode / mixed-script cases are blocked on
  it.
- **`<tref>`** (deprecated in SVG 2) — not implemented (all `e-tref-*` link/position/
  style/xml-space cases skipped).
- **`textLength` + `lengthAdjust`** — the combined form (and `lengthAdjust` parented to
  `textLength`, including the Arabic and zero cases) is not implemented.
- **Font properties** — `font` shorthand, `font-kerning` (HarfBuzz feature toggle),
  `font-size-adjust`, and the deprecated SVG 1.1 `kerning` attribute are not implemented.
- **`dominant-baseline`** — the full keyword set is not implemented (enabled coverage is
  thin).
- **SVG 2 text features** — `clipPath` on `<text>` and text children, `filter` on
  `<textPath>`, `<a>` as a text-content element, and the `<textPath>` `method="stretch"`,
  `spacing="auto"`, `side="right"`, and `path` attributes are not implemented.
- **Vertical text on a path** — `writing-mode=tb` on `<textPath>` is deferred, as is
  `transform-origin` on `textPath` (entangled with textPath layout).

### Known rendering bugs (skipped pending fix)

- Gradient stroke / radial-gradient fill on `<text>` renders incorrectly.
- Kerning is wrong on `<textPath>` and when font-size changes mid-run; rotation indices
  are misapplied across nested `<tspan>`s.
- Whitespace-only text nodes are lost across `xml:space` preserve/default nesting
  boundaries.
- Letter-spacing edge cases, and (text-full only) some non-ASCII underline, mixed-language,
  and CJK-punctuation cases still differ from the reference.

### UB / spec-ambiguous

A few cases are skipped as undefined behavior (e.g. `baseline-shift` + `rotate`
combinations) rather than bugs — see the skip reason on each.
