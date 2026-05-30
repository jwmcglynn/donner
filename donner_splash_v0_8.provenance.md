# `donner_splash_v0_8.svg` — Provenance

This file records the operations used to author the v0.8 Donner showcase splash
from the prior v0.7 logo. It mirrors the End-to-End Flow in
[`docs/design_docs/0047-v0_8_showcase.md`](docs/design_docs/0047-v0_8_showcase.md)
and exists so future releases can reproduce the showcase without guessing which
steps were used.

The design doc describes Milestone 8 as a GUI workflow. Because the editor GUI
cannot run headlessly in CI, the final asset is generated **programmatically by
driving the same merged editor code paths** (`convertTextToOutlines` and
`ExportViewportAsSvg`) through the reproducible tool
`//donner/editor/tools:generate_showcase_asset`. The resulting asset is identical
in structure to the GUI export: outlined `SVG` letters, no live `<text>`, and the
exported `id="donner-editor-overlay"` chrome.

## Source file

- `donner_splash_v0_8_editable.svg` — verbatim copy of the v0.7 `donner_splash.svg`
  with a single XML comment tagging it as the v0.8 editable intermediate. Edited
  in Donner Editor; not the final public asset.

## Operations

The final asset is produced by `//donner/editor/tools:generate_showcase_asset`,
which performs the following steps using the same code that backs the editor's
Text tool, `ConvertTextToOutlines` command, and `Export Viewport as SVG` command:

1. Load `donner_splash_v0_8_editable.svg`.
2. Insert a `<text id="showcase_svg_label" font-family="sans-serif"
   font-size="96" fill="#0b1f4d">SVG</text>` element, rooted inside the document
   `<svg>` and positioned in the lower third of the 892x512 viewBox — the same
   insertion the Text tool performs.
3. Run `convertTextToOutlines(...)` (the `ConvertTextToOutlines` command's pure
   helper) on the `SVG` text element. This replaces the live `<text>` with a
   deterministic outline group `<g id="showcase_svg_label_outlines"
   data-donner-converted-from="text">` of `<path>` glyphs, using Donner's
   renderer-facing text geometry so the outlines match the rendered text.
4. Capture a selection-chrome snapshot for the outline group and run
   `ExportViewportAsSvg(...)` with `includeSelectionOverlay = true` and a viewport
   that frames the entire splash (viewBox = document bounds, render pane = the
   splash's pixel dimensions). This emits the cropped document content plus the
   `id="donner-editor-overlay"` chrome around the outlined letters.
5. Write the export to `donner_splash_v0_8.svg`.

### Regenerate

```sh
bazel run //donner/editor/tools:generate_showcase_asset -- \
    $PWD/donner_splash_v0_8_editable.svg $PWD/donner_splash_v0_8.svg
```

The default basic-text (stb_truetype) tier is sufficient; no `--config=text-full`
is required. The text uses the engine's embedded fallback font, so the asset has
no dependency on system fonts.

## Final asset path

`donner_splash_v0_8.svg` (repo root). Stable path geometry only — no live
`<text>` element for the `SVG` letters, no dependency on system fonts.

## Donner version

`<filled at release>`

## Notes on overlay / transparency options

- Overlay group: `id="donner-editor-overlay"`, `pointer-events="none"`, clipped
  to the exported viewport.
- Selection overlay primitives included: path outlines, selection AABB, resize
  and rotate handles. No selection chips or source-reference ropes for v0.8.
- Background: transparent. The editor checkerboard is not serialized.
- Overlay styling is fixed (deterministic across themes); the showcase does not
  drift if the operator's editor theme changes locally.
